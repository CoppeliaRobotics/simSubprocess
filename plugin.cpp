#include <string>
#include <vector>
#include <boost/process.hpp>

#include "simPlusPlus/Plugin.h"
#include "config.h"
#include "plugin.h"
#include "stubs.h"

using std::string;
using std::vector;
using std::runtime_error;
namespace bp = boost::process;

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

    void exec(exec_in *in, exec_out *out)
    {
        bp::opstream sin;
        bp::ipstream sout;
        boost::filesystem::path program;
        if(in->useSearchPath)
            program = bp::search_path(in->programPath);
        else
            program = in->programPath;
        bp::child child(program, in->args, bp::std_in = sin, bp::std_out > sout);
        sin << in->stdin;
        sin.flush();
        sin.pipe().close();
        child.wait();
        out->exitCode = child.exit_code();
        std::string s(std::istreambuf_iterator<char>(sout), {});
        out->stdout = s;
    }
};

SIM_PLUGIN(PLUGIN_NAME, PLUGIN_VERSION, Plugin)
#include "stubsPlusPlus.cpp"
