#include <string>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QDir>
#include <QFileInfo>
#include <QProcess>

#include <simPlusPlus-2/Plugin.h>
#include <simPlusPlus-2/Handles.h>
#include "config.h"
#include "plugin.h"
#include "stubs.h"

using std::string;
using std::vector;
using std::runtime_error;

struct ProcessState
{
    explicit ProcessState(QProcess *p) : process(p) {}
    ~ProcessState() { delete process; }

    QProcess   *process;
    QByteArray  outBuf;      // bytes of the current, incomplete stdout line
    QByteArray  errBuf;      // same for stderr
    bool        outClosed = false;
    bool        errClosed = false;
};

class Plugin : public sim::Plugin
{
public:
    void onInit()
    {
        if(!registerScriptStuff())
            throw runtime_error("failed to register script stuff");
    }

    void onScriptStateAboutToBeDestroyed(int scriptHandle, long long scriptUid)
    {
        for(auto c : handles.find(scriptHandle))
        {
            delete handles.remove(c);
        }
    }

    template<class T>
    QString getProgram(const T &in)
    {
        QString program = QString::fromStdString(in->programPath);
        if(in->opts.useSearchPath)
        {
#ifdef _MSC_VER
            QString path = qEnvironmentVariable("PATH");
#else // _MSC_VER
            QString path = QString::fromLocal8Bit(qgetenv("PATH"));
#endif // _MSC_VER
            if(!path.isNull())
            {
                QStringList dirList(path.split(QDir::listSeparator()));
                QStringList extList;
                extList << ""
#ifdef _MSC_VER
                    << ".exe"
#endif // _MSC_VER
                    ;
                for(const auto &dir : dirList)
                {
                    QDir d(dir);
                    for(const auto &ext : extList)
                    {
                        QFileInfo f(d, program + ext);
                        if(f.exists())
                        {
                            return f.absoluteFilePath();
                        }
                    }
                }
            }
        }
        return program;
    }

    template<class T>
    QStringList getArguments(const T &in)
    {
        QStringList ret;
        for(const auto &arg : in->args)
            ret << QString::fromStdString(arg);
        return ret;
    }

    template<class T>
    void checkAuth(const T &in)
    {
        QString caller = QString("script %1").arg(in->_.scriptID);
        //auto paramData = sim::getScriptStringParamOpt(in->_.scriptID, sim_scriptstringparam_description);
        //if(paramData)
        //    caller = QString::fromStdString(*paramData);
        QString what = QString("%1 (via simSubprocess)").arg(caller);
        QString prog = getProgram(in);
        QString args = prog + " " + getArguments(in).join(" ");
        if(!sim::checkExecAuthorization(what.toStdString(), args.toStdString()))
            throw std::runtime_error("Permission denied (by user)");
    }

    void exec(exec_in *in, exec_out *out)
    {
        checkAuth(in);
        QProcess *process = new QProcess();
#ifdef _MSC_VER
        process->setCreateProcessArgumentsModifier([=] (QProcess::CreateProcessArguments *args) {
            if(in->opts.openNewConsole)
                args->flags |= CREATE_NEW_CONSOLE;
        });
#endif // _MSC_VER
        if(in->opts.workingDir)
            process->setWorkingDirectory(QString::fromStdString(*in->opts.workingDir));
        process->start(getProgram(in), getArguments(in));
        if(!process->waitForStarted(-1))
            throw std::runtime_error("waitForStarted timed out");
        process->write(QByteArray(in->input.c_str(), in->input.length()));
        process->closeWriteChannel();
        if(!process->waitForFinished(-1))
            throw std::runtime_error("waitForFinished timed out");
        QByteArray stdOutData = process->readAllStandardOutput();
        out->stdOut = std::string(stdOutData.constData(), stdOutData.length());
        QByteArray stdErrData = process->readAllStandardError();
        out->stdErr = std::string(stdErrData.constData(), stdErrData.length());
        if(process->exitStatus() == QProcess::NormalExit)
            out->exitCode = process->exitCode();
        else if(process->exitStatus() == QProcess::CrashExit)
            throw std::runtime_error("the process crashed");
        else
            throw std::runtime_error("unknown process exit status");
        delete process;
    }

    void execAsync(execAsync_in *in, execAsync_out *out)
    {
        checkAuth(in);
        QProcess *process = new QProcess();
        process->setProcessChannelMode(QProcess::ForwardedChannels);
#ifdef _MSC_VER
        process->setCreateProcessArgumentsModifier([=] (QProcess::CreateProcessArguments *args) {
            if(in->opts.openNewConsole)
                args->flags |= CREATE_NEW_CONSOLE;
        });
#endif // _MSC_VER
        if(in->opts.workingDir)
            process->setWorkingDirectory(QString::fromStdString(*in->opts.workingDir));
        process->start(getProgram(in), getArguments(in));
        if(!process->waitForStarted(-1))
            throw std::runtime_error("waitForStarted timed out");
        out->handle = handles.add(new ProcessState(process), in->_.scriptID);
    }

    void isRunning(isRunning_in *in, isRunning_out *out)
    {
        auto st = handles.get(in->handle);
        auto process = st->process;
        out->running = process->state() == QProcess::Running;
    }

    void wait(wait_in *in, wait_out *out)
    {
        auto st = handles.get(in->handle);
        auto process = st->process;
        if(!process->waitForFinished(int(1000 * in->timeout)))
            return;
        if(process->exitStatus() == QProcess::NormalExit)
            out->exitCode = process->exitCode();
        else if(process->exitStatus() == QProcess::CrashExit)
            throw std::runtime_error("the process crashed");
        else
            throw std::runtime_error("unknown process exit status");
    }

    void kill(kill_in *in, kill_out *out)
    {
        auto st = handles.get(in->handle);
        auto process = st->process;
        process->kill();
        process->waitForFinished(-1);
        delete handles.remove(st);
    }

    void getpid(getpid_in *in, getpid_out *out)
    {
        auto st = handles.get(in->handle);
        auto process = st->process;
        out->pid = process->processId();
    }

    void writeStdIn(writeStdIn_in *in, writeStdIn_out *out)
    {
        auto st = handles.get(in->handle);
        auto process = st->process;
        if(process->state() == QProcess::NotRunning)
            throw std::runtime_error("process is not running");

        QByteArray data(in->data.c_str(), int(in->data.length()));
        qint64 n = process->write(data);
        if(n < 0)
            throw std::runtime_error("write to stdin failed");
        if(n != data.length())
            throw std::runtime_error("partial write to stdin");

        // QProcess buffers writes internally and flushes via the Qt event loop.
        // In a plugin host (CoppeliaSim) we can't rely on that loop being pumped,
        // so flush explicitly. This blocks until the kernel accepts the bytes.
        if(!process->waitForBytesWritten(-1))
            throw std::runtime_error("waitForBytesWritten timed out");
    }

    void closeStdIn(closeStdIn_in *in, closeStdIn_out *out)
    {
        auto st = handles.get(in->handle);
        auto process = st->process;
        process->closeWriteChannel();
    }

private:
    sim::Handles<ProcessState*> handles{"simSubprocess.child"};
};

SIM_PLUGIN(Plugin)
#include "stubsPlusPlus.cpp"
