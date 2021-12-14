#include <string>
#include <vector>
#include <boost/process.hpp>
#include <boost/process/async.hpp>

#include "simPlusPlus/Plugin.h"
#include "simPlusPlus/Handle.h"
#include "config.h"
#include "plugin.h"
#include "stubs.h"

using std::string;
using std::vector;
using std::runtime_error;

namespace bp = boost::process;

struct Child {
    bp::child *child;
    std::future<string> out;
    std::future<string> err;
};

template<> string sim::Handle<Child>::tag() { return "simSubprocess.child"; }

class Plugin : public sim::Plugin
{
public:
    void onStart()
    {
        if(!registerScriptStuff())
            throw runtime_error("failed to register script stuff");

        setExtVersion("Subprocess Plugin");
        setBuildDate(BUILD_DATE);
    }

    void onInstancePass(const sim::InstancePassFlags &flags)
    {
        ioc.run_for(std::chrono::milliseconds{2});
    }

    void onScriptStateDestroyed(int scriptID)
    {
        for(auto c : handles.find(scriptID))
        {
            c->child->wait();
            delete c->child;
            delete handles.remove(c);
        }
    }

    template<class T>
    boost::filesystem::path getProgram(const T &in)
    {
        boost::filesystem::path program;
        if(in->useSearchPath)
            program = bp::search_path(in->programPath);
        else
            program = in->programPath;
        return program;
    }

    void exec(exec_in *in, exec_out *out)
    {
        bp::opstream sin;
        bp::ipstream sout;
        bp::child child(getProgram(in), in->args, bp::std_in = sin, bp::std_out > sout);
        sin << in->input;
        sin.flush();
        sin.pipe().close();
        child.wait();
        out->exitCode = child.exit_code();
        std::string s(std::istreambuf_iterator<char>(sout), {});
        out->output = s;
    }

    void execAsync(execAsync_in *in, execAsync_out *out)
    {
        auto c = new Child;
        c->child = new bp::child(getProgram(in), in->args, bp::std_out > c->out, bp::std_err > c->err, ioc);
        out->handle = handles.add(c, in->_.scriptID);
    }

    void isRunning(isRunning_in *in, isRunning_out *out)
    {
        auto c = handles.get(in->handle);
        out->running = c->child->running();
    }

    void wait(wait_in *in, wait_out *out)
    {
        auto c = handles.get(in->handle);
        auto t = std::chrono::milliseconds{long(1000 * in->timeout)};
        if(c->child->wait_for(t))
        {
            out->exitCode = c->child->exit_code();
        }
    }

    void terminate(terminate_in *in, terminate_out *out)
    {
        auto c = handles.get(in->handle);
        c->child->terminate();
#if 0 // this freezes the app (on macOS at least)
        c->child->wait(); //to avoid a zombie process & get the exit code
        out->exitCode = c->child->exit_code();
#endif
        delete c->child;
        delete handles.remove(c);
    }

private:
    sim::Handles<Child> handles;
    boost::asio::io_context ioc;
};

SIM_PLUGIN(PLUGIN_NAME, PLUGIN_VERSION, Plugin)
#include "stubsPlusPlus.cpp"
