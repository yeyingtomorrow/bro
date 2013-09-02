// See the file "COPYING" in the main distribution directory for copyright.

#include <sstream>
#include <dirent.h>
#include <glob.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>

#include "Manager.h"

#include "../Reporter.h"

using namespace plugin;

string Manager::current_dir;
string Manager::current_sopath;

Manager::Manager()
	{
	init = false;
	}

Manager::~Manager()
	{
	assert(! init);
	}

void Manager::LoadPluginsFrom(const string& dir)
	{
	assert(! init);

	if ( dir.empty() )
		return;

	if ( dir.find(":") != string::npos )
		{
		// Split at ":".
		std::stringstream s(dir);
		std::string d;

		while ( std::getline(s, d, ':') )
			LoadPluginsFrom(d);

		return;
		}

	if ( ! is_dir(dir) )
		{
		DBG_LOG(DBG_PLUGINS, "Not a valid plugin directory: %s", dir.c_str());
		return;
		}

	int rc = LoadPlugin(dir);

	if ( rc >= 0 )
		return;

	DBG_LOG(DBG_PLUGINS, "Searching directory %s recursively for plugins", dir.c_str());

	DIR* d = opendir(dir.c_str());

	if ( ! d )
		{
		DBG_LOG(DBG_PLUGINS, "Cannot open directory %s", dir.c_str());
		return;
		}

	bool found = false;

	struct dirent *dp;

	while ( (dp = readdir(d)) )
		{
		struct stat st;

		if ( strcmp(dp->d_name, "..") == 0
		     || strcmp(dp->d_name, ".") == 0 )
			continue;

		string path = dir + "/" + dp->d_name;

		if( stat(path.c_str(), &st) < 0 )
			{
			DBG_LOG(DBG_PLUGINS, "Cannot stat %s: %s", path.c_str(), strerror(errno));
			continue;
			}

		if ( st.st_mode & S_IFDIR )
			LoadPluginsFrom(path);
		}
	}

int Manager::LoadPlugin(const std::string& dir)
	{
	assert(! init);

	// Check if it's a plugin dirctory.
	if ( ! is_file(dir + "/__bro_plugin__") )
		return -1;

	DBG_LOG(DBG_PLUGINS, "Loading plugin from %s", dir.c_str());

	// Add the "scripts" directory to BROPATH.
	string scripts = dir + "/scripts";

	if ( is_dir(scripts) )
		{
		DBG_LOG(DBG_PLUGINS, "  Adding %s to BROPATH", scripts.c_str());
		add_to_bro_path(scripts);
		}

	// Load dylib/scripts/__load__.bro automatically.
	string dyinit = dir + "/dylib/scripts/__load__.bro";

	if ( is_file(dyinit) )
		{
		DBG_LOG(DBG_PLUGINS, "  Adding %s for loading", dyinit.c_str());
		add_input_file(dyinit.c_str());
		}

	// Load scripts/__load__.bro automatically.
	string init = scripts + "/__load__.bro";

	if ( is_file(init) )
		{
		DBG_LOG(DBG_PLUGINS, "  Adding %s for loading", init.c_str());
		add_input_file(init.c_str());
		}

	// Load shared libraries.

	string dypattern = dir + "/dylib/*." + HOST_ARCHITECTURE + SHARED_LIBRARY_SUFFIX;

	DBG_LOG(DBG_PLUGINS, "  Searching for shared libraries %s", dypattern.c_str());

	glob_t gl;

	if ( glob(dypattern.c_str(), 0, 0, &gl) == 0 )
		{
		for ( size_t i = 0; i < gl.gl_pathc; i++ )
			{
			const char* path = gl.gl_pathv[i];

			current_dir = dir;
			current_sopath = path;
			void* hdl = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
			current_dir.clear();
			current_sopath.clear();

			if ( ! hdl )
				{
				const char* err = dlerror();
				reporter->FatalError("cannot load plugin library %s: %s", path, err ? err : "<unknown error>");
				}

			DBG_LOG(DBG_PLUGINS, "  Loaded %s", path);
			}
		}

	else
		{
		DBG_LOG(DBG_PLUGINS, "  No shared library found");
		return 1;
		}

	return 1;
	}

static bool plugin_cmp(const Plugin* a, const Plugin* b)
	{
	return a->Name() < b->Name();
	}

bool Manager::RegisterPlugin(Plugin *plugin)
	{
	Manager::PluginsInternal()->push_back(plugin);

	if ( current_dir.size() && current_sopath.size() )
		plugin->SetPluginLocation(current_dir.c_str(), current_sopath.c_str());

	// Sort plugins by name to make sure we have a deterministic order.
	PluginsInternal()->sort(plugin_cmp);

	return true;
	}

void Manager::InitPreScript()
	{
	assert(! init);

	for ( plugin_list::iterator i = Manager::PluginsInternal()->begin(); i != Manager::PluginsInternal()->end(); i++ )
		{
		Plugin* plugin = *i;

		plugin->InitPreScript();

		// Track the file extensions the plugin can handle.
		std::stringstream ext(plugin->FileExtensions());

		// Split at ":".
		std::string e;

		while ( std::getline(ext, e, ':') )
			{
			DBG_LOG(DBG_PLUGINS, "Plugin %s handles *.%s", plugin->Name(), e.c_str());
			extensions.insert(std::make_pair(e, plugin));
			}
		}

	init = true;
	}

void Manager::InitBifs()
	{
	for ( plugin_list::iterator i = Manager::PluginsInternal()->begin(); i != Manager::PluginsInternal()->end(); i++ )
		(*i)->InitBifs();
	}

void Manager::InitPostScript()
	{
	assert(init);

	for ( plugin_list::iterator i = Manager::PluginsInternal()->begin(); i != Manager::PluginsInternal()->end(); i++ )
		(*i)->InitPostScript();
	}

void Manager::FinishPlugins()
	{
	assert(init);

	for ( plugin_list::iterator i = Manager::PluginsInternal()->begin(); i != Manager::PluginsInternal()->end(); i++ )
		{
		(*i)->Done();
//		delete *i;
		}

	Manager::PluginsInternal()->clear();

	init = false;
	}

int Manager::TryLoadFile(const char* file)
	{
	assert(file);
	const char* ext = strrchr(file, '.');

	if ( ! ext )
		return -1;

	extension_map::iterator i = extensions.find(++ext);
	if ( i == extensions.end() )
		return -1;

	Plugin* plugin = i->second;

	DBG_LOG(DBG_PLUGINS, "Loading %s with %s", file, plugin->Name());

	if ( i->second->LoadFile(file) )
		return 1;

	return 0;
	}

Manager::plugin_list Manager::Plugins() const
	{
	return *Manager::PluginsInternal();
	}

Manager::plugin_list* Manager::PluginsInternal()
	{
	static plugin_list* plugins = 0;

	if ( ! plugins )
		plugins = new plugin_list;

	return plugins;
	}
