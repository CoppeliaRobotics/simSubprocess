#include <string>
#include <vector>
#include <chrono>

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
    bool        terminating = false;
};

struct KillEntry
{
    ProcessState *state;
    std::chrono::steady_clock::time_point deadline;
    double killDelay; // seconds between terminate() and kill()
    int    stage;    // 0 = awaiting initial delay
                     // 1 = awaiting grace1
                     // 2 = escalated, waiting for exit
};

class Plugin : public sim::Plugin
{
public:
    void onInit() override
    {
        if(!registerScriptStuff())
            throw runtime_error("failed to register script stuff");
    }

    void onCleanup() override
    {
        for(auto st : handles.all())
        {
            killAndRemoveHandle(st);
        }
    }

    void onScriptStateAboutToBeDestroyed(int scriptHandle, long long scriptUid) override
    {
        for(auto st : handles.find(scriptHandle))
        {
            killAndRemoveHandle(st);
        }
    }

    void onInstancePass(const sim::InstancePassFlags &flags) override
    {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now();

        for(auto it = killQueue.begin(); it != killQueue.end(); )
        {
            KillEntry &e = *it;
            QProcess  *p = e.state->process;

            // Non-blocking pump: updates state/exit-code and drains the pipes
            // (QProcess needs explicit pumping when there is no Qt event loop).
            p->waitForFinished(0);

            if(p->state() == QProcess::NotRunning)
            {
                // Reaped and clean: drop the handle.
                delete handles.remove(e.state);
                it = killQueue.erase(it);
                continue;
            }

            if(e.stage < 2 && now >= e.deadline)
            {
                if(e.stage == 0)
                {
                    // Stage 1 — graceful
                    p->terminate();
                    e.stage = 1;
                    e.deadline = now + std::chrono::duration_cast<clock::duration>(
                        std::chrono::duration<double>(e.killDelay));
                }
                else // e.stage == 1
                {
                    // Stage 2 — forceful
                    p->kill();
                    e.stage = 2;   // no further deadline; just wait for exit
                }
            }
            ++it;
        }
    }

    void killAndRemoveHandle(ProcessState *st)
    {
        killQueue.erase(
            std::remove_if(killQueue.begin(), killQueue.end(),
                [st](const KillEntry &e) { return e.state == st; }),
            killQueue.end());

        QProcess *p = st->process;
        if(p->state() != QProcess::NotRunning)
        {
            p->terminate();
            if(!p->waitForFinished(500))
            {
                p->kill();
                p->waitForFinished(-1);
            }
        }

        delete handles.remove(st);
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
        if(in->opts.captureOutput)
            process->setProcessChannelMode(QProcess::SeparateChannels);
        else
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
        out->running = st->process->state() == QProcess::Running;
    }

    void wait(wait_in *in, wait_out *out)
    {
        auto st = handles.get(in->handle);
        if(!st->process->waitForFinished(int(1000 * in->timeout)))
            return;
        if(st->process->exitStatus() == QProcess::NormalExit)
            out->exitCode = st->process->exitCode();
        else if(st->process->exitStatus() == QProcess::CrashExit)
            throw std::runtime_error("the process crashed");
        else
            throw std::runtime_error("unknown process exit status");
    }

    void kill(kill_in *in, kill_out *out)
    {
        auto st = handles.get(in->handle);

        // if called while a KillEntry is pending, it should cancel the schedule
        // and take over:
        killQueue.erase(
            std::remove_if(killQueue.begin(), killQueue.end(),
                [st](const KillEntry &e) { return e.state == st; }),
            killQueue.end());

        st->process->kill();
        st->process->waitForFinished(-1);

        delete handles.remove(st);
    }

    void killEx(killEx_in *in, killEx_out *)
    {
        auto st = handles.get(in->handle);

        if(st->terminating)
            throw std::runtime_error("killEx: process is already being terminated");
        st->terminating = true;

        using clock = std::chrono::steady_clock;
        KillEntry e;
        e.state = st;
        e.killDelay = in->killDelay;
        e.stage = 0;
        e.deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(in->delay));
        killQueue.push_back(e);
    }


    void getpid(getpid_in *in, getpid_out *out)
    {
        auto st = handles.get(in->handle);
        out->pid = st->process->processId();
    }

    void writeStdIn(writeStdIn_in *in, writeStdIn_out *out)
    {
        auto st = handles.get(in->handle);
        if(st->process->state() == QProcess::NotRunning)
            throw std::runtime_error("process is not running");

        QByteArray data(in->data.c_str(), int(in->data.length()));
        qint64 n = st->process->write(data);
        if(n < 0)
            throw std::runtime_error("write to stdin failed");
        if(n != data.length())
            throw std::runtime_error("partial write to stdin");

        // QProcess buffers writes internally and flushes via the Qt event loop.
        // In a plugin host (CoppeliaSim) we can't rely on that loop being pumped,
        // so flush explicitly. This blocks until the kernel accepts the bytes.
        if(!st->process->waitForBytesWritten(-1))
            throw std::runtime_error("waitForBytesWritten timed out");
    }

    void closeStdIn(closeStdIn_in *in, closeStdIn_out *out)
    {
        auto st = handles.get(in->handle);
        st->process->closeWriteChannel();
    }

    static void extractLines(QByteArray &buf, std::vector<std::string> &outLines, int maxLines)
    {
        int start = 0;
        int emitted = 0;
        while(emitted < maxLines)
        {
            int nl = buf.indexOf('\n', start);
            if(nl < 0) break;
            int len = nl - start;
            if(len > 0 && buf[start + len - 1] == '\r') --len;   // CRLF
            outLines.emplace_back(buf.constData() + start, len);
            start  = nl + 1;
            ++emitted;
        }
        if(start > 0)
            buf.remove(0, start);
    }

    void poll(poll_in *in, poll_out *out)
    {
        auto *st = handles.get(in->handle);
        QProcess *p = st->process;

        if(p->state() == QProcess::Running)
        {
            // waitForReadyRead() drives Qt's pipe reads from the current read
            // channel only, so pump each channel once. With timeout 0 it is a
            // non-blocking poll: returns immediately with whatever is available.
            p->setReadChannel(QProcess::StandardOutput);
            p->waitForReadyRead(0);
            p->setReadChannel(QProcess::StandardError);
            p->waitForReadyRead(0);
            p->setReadChannel(QProcess::StandardOutput);
        }
        else
        {
            // Not running: make sure the exit status / code are final and any
            // remaining bytes have been drained by QProcess.
            p->waitForFinished(0);
        }

        // Move whatever Qt has buffered into our line buffers.
        st->outBuf.append(p->readAllStandardOutput());
        st->errBuf.append(p->readAllStandardError());

        extractLines(st->outBuf, out->stdOut, in->maxLines);
        extractLines(st->errBuf, out->stdErr, in->maxLines);

        // EOF per channel: process gone and no partial line left over.
        if(!st->outClosed && p->state() == QProcess::NotRunning && st->outBuf.isEmpty())
            st->outClosed = true;
        if(!st->errClosed && p->state() == QProcess::NotRunning && st->errBuf.isEmpty())
            st->errClosed = true;

        out->stdOutClosed = st->outClosed;
        out->stdErrClosed = st->errClosed;
        out->running      = (p->state() != QProcess::NotRunning);
        out->exitCode     = (p->state() == QProcess::NotRunning &&
                            p->exitStatus() == QProcess::NormalExit)
                            ? p->exitCode() : 0;
    }

private:
    sim::Handles<ProcessState*> handles{"simSubprocess.child"};
    std::vector<KillEntry> killQueue;
};

SIM_PLUGIN(Plugin)
#include "stubsPlusPlus.cpp"
