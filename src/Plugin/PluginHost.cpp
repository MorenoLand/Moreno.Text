#include "Plugin/PluginHost.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <functional>
#include <regex>
#include <sstream>
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace fs = std::filesystem;

std::string PluginHost::legacyApiName() { return std::string("sub") + "lime"; }

std::vector<PluginHost::Script> PluginHost::discoverScripts(const std::string& packagesDir) {
    std::vector<Script> scripts;
    try {
        if (!fs::exists(packagesDir)) return scripts;
        for (auto& packageEntry : fs::directory_iterator(packagesDir)) {
            if (!packageEntry.is_directory()) continue;
            std::string package = packageEntry.path().filename().string();
            if (package == "User") continue;
            for (auto& fileEntry : fs::directory_iterator(packageEntry.path())) {
                if (!fileEntry.is_regular_file() || fileEntry.path().extension() != ".py") continue;
                scripts.push_back({package, fileEntry.path().string()});
            }
        }
    } catch (...) {}
    return scripts;
}

std::string PluginHost::commandNameFromClass(const std::string& className) {
    std::string base = className;
    const std::string suffix = "Command";
    if (base.size() > suffix.size() && base.substr(base.size() - suffix.size()) == suffix) base.resize(base.size() - suffix.size());
    std::string out;
    for (size_t i = 0; i < base.size(); ++i) {
        char c = base[i];
        if (i > 0 && std::isupper((unsigned char)c) && (std::islower((unsigned char)base[i - 1]) || (i + 1 < base.size() && std::islower((unsigned char)base[i + 1])))) out += '_';
        out += (char)std::tolower((unsigned char)c);
    }
    return out;
}

std::string PluginHost::commandCaptionFromName(const std::string& name) {
    std::string out = "Plugin: ";
    bool cap = true;
    for (char c : name) {
        if (c == '_') { out += ' '; cap = true; continue; }
        out += cap ? (char)std::toupper((unsigned char)c) : c;
        cap = false;
    }
    return out;
}

std::string PluginHost::hostExecutablePath(const PluginHostPaths& paths) {
    return (fs::path(paths.exeDir) / "moreno_plugin_host.exe").string();
}

#ifdef _WIN32
static fs::path pluginHostPidPath(const PluginHostPaths& paths) {
    return fs::path(paths.dataDir) / "Local" / "plugin_host.pid";
}

static bool terminateProcessIfSameHost(DWORD pid, const fs::path& hostPath, const std::function<void(const std::string&)>& appendLog) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    bool terminated = false;
    DWORD exitCode = 0;
    if (GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE) {
        char path[MAX_PATH] = {};
        DWORD pathLen = MAX_PATH;
        bool sameHost = QueryFullProcessImageNameA(process, 0, path, &pathLen) &&
            fs::equivalent(fs::path(path), hostPath);
        if (sameHost) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 1500);
            appendLog("[plugin-host] stopped stale host pid " + std::to_string(pid));
            terminated = true;
        }
    }
    CloseHandle(process);
    return terminated;
}

static void stopPreviousPluginHost(const PluginHostPaths& paths, const fs::path& hostPath, const std::function<void(const std::string&)>& appendLog) {
    fs::path pidPath = pluginHostPidPath(paths);
        try {
        if (!fs::exists(hostPath)) return;
        if (fs::exists(pidPath)) {
            std::ifstream f(pidPath, std::ios::binary);
            std::string text;
            std::getline(f, text);
            if (!text.empty()) terminateProcessIfSameHost(static_cast<DWORD>(std::stoul(text)), hostPath, appendLog);
            fs::remove(pidPath);
        }

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"moreno_plugin_host.exe") == 0) {
                    terminateProcessIfSameHost(entry.th32ProcessID, hostPath, appendLog);
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    } catch (...) {
    }
}

static void writePluginHostPid(const PluginHostPaths& paths, DWORD pid) {
    try {
        fs::path pidPath = pluginHostPidPath(paths);
        fs::create_directories(pidPath.parent_path());
        std::ofstream f(pidPath, std::ios::binary | std::ios::trunc);
        f << pid;
    } catch (...) {
    }
}
#endif

std::vector<PluginHost::Command> PluginHost::discoverCommandPaletteEntries(const std::string& packagesDir) {
    std::vector<Command> commands;
    try {
        if (!fs::exists(packagesDir)) return commands;
        for (auto& packageEntry : fs::directory_iterator(packagesDir)) {
            if (!packageEntry.is_directory()) continue;
            fs::path commandFile = packageEntry.path() / ("Default." + legacyApiName() + "-commands");
            if (!fs::exists(commandFile)) continue;
            std::ifstream f(commandFile, std::ios::binary);
            if (!f) continue;
            nlohmann::json j;
            f >> j;
            if (!j.is_array()) continue;
            for (auto& entry : j) {
                if (!entry.is_object()) continue;
                std::string name = entry.value("command", "");
                std::string caption = entry.value("caption", "");
                if (name.empty()) continue;
                if (caption.empty()) caption = commandCaptionFromName(name);
                commands.push_back({caption, name, commandFile.string()});
            }
        }
    } catch (...) {}
    return commands;
}

std::vector<PluginHost::Command> PluginHost::discoverCommands(const std::vector<Script>& scripts) {
    std::vector<Command> commands;
    std::set<std::string> seen;
    if (!scripts.empty()) {
        fs::path packagesDir = fs::path(scripts.front().path).parent_path().parent_path();
        commands = discoverCommandPaletteEntries(packagesDir.string());
        for (auto& command : commands) seen.insert(command.name);
    }
    std::regex re(R"(class\s+([A-Za-z_][A-Za-z0-9_]*Command)\s*\([^)]*(?:TextCommand|WindowCommand|ApplicationCommand))");
    for (auto& script : scripts) {
        std::ifstream f(script.path, std::ios::binary);
        if (!f) continue;
        std::ostringstream ss; ss << f.rdbuf();
        std::string text = ss.str();
        for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
            std::string name = commandNameFromClass((*it)[1].str());
            if (seen.find(name) != seen.end()) continue;
            seen.insert(name);
            commands.push_back({commandCaptionFromName(name), name, script.path});
        }
    }
    return commands;
}

bool PluginHost::writeBootstrapFiles(const PluginHostPaths& paths) {
    try {
        fs::path lib = fs::path(paths.libDir) / "python38";
        fs::create_directories(lib);
        std::string api = legacyApiName();
        {
            std::ofstream f(lib / (api + ".py"), std::ios::binary);
            if (!f) return false;
            f << "import inspect, json, os, sys, threading, time, uuid\n"
              << "ENCODED_POSITION = 0\n"
              << "DRAW_NO_FILL = 1\n"
              << "DRAW_NO_OUTLINE = 2\n"
              << "DRAW_SOLID_UNDERLINE = 4\n"
              << "DRAW_STIPPLED_UNDERLINE = 8\n"
              << "DRAW_SQUIGGLY_UNDERLINE = 16\n"
              << "HIDE_ON_MOUSE_MOVE_AWAY = 32\n"
              << "COOPERATE_WITH_AUTO_COMPLETE = 64\n"
              << "HOVER_TEXT = 1\n"
              << "INHIBIT_WORD_COMPLETIONS = 1\n"
              << "INHIBIT_EXPLICIT_COMPLETIONS = 2\n"
              << "KIND_ID_NAMESPACE = 1\n"
              << "KIND_ID_TYPE = 2\n"
              << "KIND_ID_VARIABLE = 3\n"
              << "KIND_VARIABLE = (KIND_ID_VARIABLE, 'v', 'Variable')\n"
              << "KIND_FUNCTION = (4, 'f', 'Function')\n"
              << "HIDDEN = 0\n"
              << "TRANSIENT = 1\n"
              << "PERSISTENT = 2\n"
              << "_settings_cache = {}\n"
              << "_timers = []\n"
              << "_thread_init = threading.Thread.__init__\n"
              << "def _moreno_thread_init(self, *args, **kwargs):\n"
              << "    if kwargs.get('daemon') is True: kwargs['daemon'] = False\n"
              << "    _thread_init(self, *args, **kwargs)\n"
              << "threading.Thread.__init__ = _moreno_thread_init\n"
              << "def packages_path(): return os.environ.get('MORENO_PACKAGES_DIR', '')\n"
              << "def installed_packages_path(): return os.environ.get('MORENO_INSTALLED_PACKAGES_PATH', '')\n"
              << "def cache_path(): return os.environ.get('MORENO_CACHE_PATH', '')\n"
              << "def _settings_candidates(name):\n"
              << "    pkgs=packages_path(); out=[]\n"
              << "    if pkgs:\n"
              << "        out.append(os.path.join(pkgs, 'User', name))\n"
              << "        for pkg in os.listdir(pkgs) if os.path.isdir(pkgs) else []:\n"
              << "            out.append(os.path.join(pkgs, pkg, name))\n"
              << "    return out\n"
              << "def _load_json_with_comments(path):\n"
              << "    text=open(path, 'r', encoding='utf-8').read(); out=[]\n"
              << "    in_str=False; esc=False; i=0\n"
              << "    while i < len(text):\n"
              << "        ch=text[i]\n"
              << "        if in_str:\n"
              << "            out.append(ch); esc=(ch=='\\\\' and not esc); in_str = False if (ch=='\"' and not esc) else in_str; i+=1; continue\n"
              << "        if ch=='\"': in_str=True; out.append(ch); i+=1; continue\n"
              << "        if ch=='/' and i+1 < len(text) and text[i+1]=='/':\n"
              << "            while i < len(text) and text[i] not in '\\r\\n': i+=1\n"
              << "            continue\n"
              << "        out.append(ch); i+=1\n"
              << "    return json.loads(''.join(out))\n"
              << "class Settings:\n"
              << "    def __init__(self, name): self.name=name; self.data={}; self._load()\n"
              << "    def _load(self):\n"
              << "        for path in _settings_candidates(self.name):\n"
              << "            try:\n"
              << "                self.data.update(_load_json_with_comments(path))\n"
              << "            except Exception: pass\n"
              << "    def get(self, key, default=None): return self.data.get(key, default)\n"
              << "    def set(self, key, value): self.data[key]=value\n"
              << "    def erase(self, key): self.data.pop(key, None)\n"
              << "    def has(self, key): return key in self.data\n"
              << "    def to_dict(self): return dict(self.data)\n"
              << "def load_settings(name):\n"
              << "    if name not in _settings_cache: _settings_cache[name]=Settings(name)\n"
              << "    return _settings_cache[name]\n"
              << "def save_settings(name):\n"
              << "    settings=load_settings(name); pkgs=packages_path(); path=os.path.join(pkgs, 'User', name)\n"
              << "    os.makedirs(os.path.dirname(path), exist_ok=True)\n"
              << "    with open(path, 'w', encoding='utf-8') as fp: json.dump(settings.data, fp, indent=2)\n"
              << "def status_message(msg): print('[status]', msg)\n"
              << "def error_message(msg): print('[error]', msg)\n"
              << "def message_dialog(msg): print('[message]', msg)\n"
              << "def ok_cancel_dialog(msg, ok_title='OK'): print('[ok-cancel]', msg); return False\n"
              << "def set_clipboard(text): print('[clipboard]', text)\n"
              << "def set_timeout(fn, delay=0):\n"
              << "    if delay <= 0: fn(); return None\n"
              << "    t=threading.Timer(delay/1000.0, fn); t.daemon=True; t.start(); _timers.append(t); return t\n"
              << "def cancel_timeout(timer):\n"
              << "    try: timer.cancel()\n"
              << "    except Exception: pass\n"
              << "_view_registry = {}\n"
              << "def _invalidate_view_id(view_id):\n"
              << "    view=_view_registry.pop(int(view_id), None)\n"
              << "    if not view: return False\n"
              << "    view._valid=False; view._watching=False\n"
              << "    if view.window_obj:\n"
              << "        view.window_obj._views=[v for v in view.window_obj._views if v is not view]\n"
              << "        if getattr(view.window_obj, '_active_view', None) is view: view.window_obj._active_view=None\n"
              << "    return True\n"
              << "def _handle_bridge_lifecycle_request(data):\n"
              << "    if (data or {}).get('type') == 'view_closed':\n"
              << "        return _invalidate_view_id(int((data or {}).get('id', -1)))\n"
              << "    return False\n"
              << "def _snake_from_class(name):\n"
              << "    raw=name[:-7] if name.endswith('Command') else name; snake=''\n"
              << "    for i,ch in enumerate(raw):\n"
              << "        if i and ch.isupper() and (raw[i-1].islower() or (i+1 < len(raw) and raw[i+1].islower())): snake += '_'\n"
              << "        snake += ch.lower()\n"
              << "    return snake\n"
              << "def _find_command_class(cmd):\n"
              << "    import inspect, sys\n"
              << "    for mod in list(sys.modules.values()):\n"
              << "        if not mod: continue\n"
              << "        for obj in vars(mod).values():\n"
              << "            if inspect.isclass(obj) and obj.__name__.endswith('Command') and _snake_from_class(obj.__name__) == cmd: return obj\n"
              << "    return None\n"
              << "def _ensure_view_api(view):\n"
              << "    if view is None: return view\n"
              << "    if not hasattr(view, 'retarget'):\n"
              << "        def _retarget(path, _view=view):\n"
              << "            _view.settings().set('file_name', str(path))\n"
              << "            emit=getattr(_view, '_emit', None)\n"
              << "            if emit: emit(False)\n"
              << "        setattr(view, 'retarget', _retarget)\n"
              << "    return view\n"
              << "class Selection:\n"
              << "    def __init__(self, view): self.view=view; self.regions=[]\n"
              << "    def clear(self): self.regions.clear()\n"
              << "    def add(self, region): self.regions.append(region)\n"
              << "    def __len__(self): return len(self.regions)\n"
              << "    def __getitem__(self, index): return self.regions[index]\n"
              << "class View:\n"
              << "    _next_id=1\n"
              << "    def __init__(self, window):\n"
              << "        self.window_obj=window; self.id=View._next_id; View._next_id+=1; _view_registry[self.id]=self; self.text=''; self.title='untitled'; self._settings=Settings('<view>'); self._sel=Selection(self); self._read_only=False; self._dirty=False; self._valid=True; self._statuses={}; self._seen_bridge=set(); self._watching=True; self._watch_thread=threading.Thread(target=self._watch_bridge, daemon=False); self._watch_thread.start()\n"
              << "    def is_valid(self): return self._valid\n"
              << "    def is_dirty(self): return self._dirty\n"
              << "    def settings(self): return self._settings\n"
              << "    def name(self): return self.title\n"
              << "    def set_name(self, name): self.title=str(name); self._emit(False)\n"
              << "    def file_name(self): return self._settings.get('file_name')\n"
              << "    def retarget(self, path): self._settings.set('file_name', str(path)); self._emit(False)\n"
              << "    def set_scratch(self, value): self._settings.set('scratch', bool(value))\n"
              << "    def assign_syntax(self, value): self._settings.set('syntax', value)\n"
              << "    def set_syntax_file(self, value): self.assign_syntax(value)\n"
              << "    def set_read_only(self, value): self._read_only=bool(value)\n"
              << "    def set_status(self, key, value): self._statuses[str(key)]=str(value); self._emit(False)\n"
              << "    def erase_status(self, key): self._statuses.pop(str(key), None); self._emit(False)\n"
              << "    def add_regions(self, key, regions, scope='', icon='', flags=0): self._settings.set('regions_'+str(key), [repr(r) for r in (regions or [])])\n"
              << "    def erase_regions(self, key): self._settings.erase('regions_'+str(key))\n"
              << "    def size(self): return len(self.text)\n"
              << "    def sel(self): return self._sel\n"
              << "    def insert(self, edit, point, characters): self.text=self.text[:point]+str(characters)+self.text[point:]; self._dirty=True; self._set_cursor(point+len(str(characters))); self._emit(False)\n"
              << "    def replace(self, edit, region, characters): self.text=self.text[:region.begin()]+str(characters)+self.text[region.end():]; self._dirty=True; self._set_cursor(region.begin()+len(str(characters))); self._emit(False)\n"
              << "    def substr(self, region): return self.text[region.begin():region.end()]\n"
              << "    def line(self, point):\n"
              << "        point=max(0,min(int(point),len(self.text))); start=self.text.rfind('\\n',0,point)+1; end=self.text.find('\\n',point)\n"
              << "        if end < 0: end=len(self.text)\n"
              << "        return Region(start,end)\n"
              << "    def _set_cursor(self, point): self._sel.clear(); self._sel.add(Region(point, point))\n"
              << "    def run_command(self, name, args=None):\n"
              << "        args=args or {}\n"
              << "        if name == 'mark_clean': self._dirty=False; self._emit(False); return\n"
              << "        if name == 'move_to': self._emit(False); return\n"
              << "        cls=_find_command_class(name)\n"
              << "        if not cls: print('[view-command]', name, args); return\n"
              << "        inst=cls(self)\n"
              << "        try: inst.run(None, **args)\n"
              << "        except TypeError: inst.run(None)\n"
              << "    def _dispatch_text_command(self, command, args):\n"
              << "        plugin_mod=__import__('sub'+'lime_plugin')\n"
              << "        for mod in list(sys.modules.values()):\n"
              << "            if not mod: continue\n"
              << "            for obj in vars(mod).values():\n"
              << "                try: is_listener=inspect.isclass(obj) and issubclass(obj, plugin_mod.EventListener) and obj is not plugin_mod.EventListener\n"
              << "                except Exception: is_listener=False\n"
              << "                if not is_listener: continue\n"
              << "                cb=getattr(obj(), 'on_text_command', None)\n"
              << "                if not cb: continue\n"
              << "                result=cb(self, command, args)\n"
              << "                if result:\n"
              << "                    name, new_args=result\n"
              << "                    self.run_command(name, new_args or {})\n"
              << "                    return\n"
              << "        self.run_command(command, args or {})\n"
              << "    def _dispatch_post_save(self):\n"
              << "        plugin_mod=__import__('sub'+'lime_plugin')\n"
              << "        for mod in list(sys.modules.values()):\n"
              << "            if not mod: continue\n"
              << "            for obj in vars(mod).values():\n"
              << "                try: is_listener=inspect.isclass(obj) and issubclass(obj, plugin_mod.EventListener) and obj is not plugin_mod.EventListener\n"
              << "                except Exception: is_listener=False\n"
              << "                if not is_listener: continue\n"
              << "                cb=getattr(obj(), 'on_post_save', None)\n"
              << "                if not cb: continue\n"
              << "                try: cb(self)\n"
              << "                except Exception as e: print('[post-save-error]', e)\n"
              << "    def _dispatch_window_command(self, command, args):\n"
              << "        if self.window_obj: self.window_obj._active_view=self\n"
              << "        plugin_mod=__import__('sub'+'lime_plugin')\n"
              << "        for mod in list(sys.modules.values()):\n"
              << "            if not mod: continue\n"
              << "            for obj in vars(mod).values():\n"
              << "                try: is_listener=inspect.isclass(obj) and issubclass(obj, plugin_mod.EventListener) and obj is not plugin_mod.EventListener\n"
              << "                except Exception: is_listener=False\n"
              << "                if not is_listener: continue\n"
              << "                cb=getattr(obj(), 'on_window_command', None)\n"
              << "                if not cb: continue\n"
              << "                result=cb(self.window_obj, command, args or {})\n"
              << "                if result:\n"
              << "                    name, new_args=result\n"
              << "                    self.window_obj.run_command(name, new_args or {})\n"
              << "                    return\n"
              << "        if self.window_obj: self.window_obj.run_command(command, args or {})\n"
              << "    def _handle_bridge_request(self, path):\n"
              << "        try:\n"
              << "            with open(path, 'r', encoding='utf-8') as fp: data=json.load(fp)\n"
              << "            req_type=data.get('type')\n"
              << "            if int(data.get('id',-1))!=self.id: return\n"
              << "            if req_type == 'view_closed':\n"
              << "                _invalidate_view_id(self.id)\n"
              << "                return\n"
              << "            if req_type not in ('view_text_command','view_window_command') or not self._valid: return\n"
              << "            self.text=str(data.get('content',''))\n"
              << "            self._dirty=True\n"
              << "            self._set_cursor(int(data.get('cursor', len(self.text))))\n"
              << "            if req_type == 'view_text_command': self._dispatch_text_command(str(data.get('command','insert')), data.get('args') or {})\n"
              << "            else: self._dispatch_window_command(str(data.get('command','')), data.get('args') or {})\n"
              << "        except Exception as e: print('[bridge-view-command-error]', e)\n"
              << "    def _watch_bridge(self):\n"
              << "        bridge=os.environ.get('MORENO_BRIDGE_DIR','')\n"
              << "        while self._watching:\n"
              << "            try:\n"
              << "                if bridge and os.path.isdir(bridge):\n"
              << "                    for name in os.listdir(bridge):\n"
              << "                        if not name.endswith('.request.json'): continue\n"
              << "                        path=os.path.join(bridge,name)\n"
              << "                        if path in self._seen_bridge: continue\n"
              << "                        self._seen_bridge.add(path); self._handle_bridge_request(path)\n"
              << "            except Exception as e: print('[bridge-watch-error]', e)\n"
              << "            time.sleep(0.05)\n"
              << "    def _emit(self, focus=False):\n"
              << "        bridge=os.environ.get('MORENO_BRIDGE_DIR','')\n"
              << "        if not bridge: return\n"
              << "        os.makedirs(bridge, exist_ok=True)\n"
              << "        rid=str(uuid.uuid4()); request=os.path.join(bridge, rid + '.request.json')\n"
              << "        with open(request, 'w', encoding='utf-8') as fp: json.dump({'type':'view_update','id':self.id,'name':self.title,'content':self.text,'focus':bool(focus),'settings':self._settings.to_dict()}, fp)\n"
              << "class Window:\n"
              << "    def show_quick_panel(self, items, on_done, *args, **kwargs):\n"
              << "        bridge=os.environ.get('MORENO_BRIDGE_DIR','')\n"
              << "        if not bridge:\n"
              << "            on_done(-1); return\n"
              << "        os.makedirs(bridge, exist_ok=True)\n"
              << "        rid=str(uuid.uuid4())\n"
              << "        response=os.path.join(bridge, rid + '.response.json')\n"
              << "        request=os.path.join(bridge, rid + '.request.json')\n"
              << "        labels=[]\n"
              << "        for item in items:\n"
              << "            labels.append(' '.join(map(str, item)) if isinstance(item, (list, tuple)) else str(item))\n"
              << "        print('[quick-panel]', len(labels), 'items')\n"
              << "        with open(request, 'w', encoding='utf-8') as fp: json.dump({'type':'quick_panel','items':labels,'response':response}, fp)\n"
              << "        deadline=time.time()+120\n"
              << "        while time.time() < deadline:\n"
              << "            if os.path.exists(response):\n"
              << "                try:\n"
              << "                    with open(response, 'r', encoding='utf-8') as fp: index=int(json.load(fp).get('index', -1))\n"
              << "                    try: os.remove(response)\n"
              << "                    except Exception: pass\n"
              << "                except Exception: index=-1\n"
              << "                on_done(index); return\n"
              << "            time.sleep(0.05)\n"
              << "        on_done(-1)\n"
              << "    def show_input_panel(self, caption, initial_text, on_done, on_change=None, on_cancel=None):\n"
              << "        bridge=os.environ.get('MORENO_BRIDGE_DIR','')\n"
              << "        if not bridge:\n"
              << "            if on_cancel: on_cancel()\n"
              << "            return None\n"
              << "        os.makedirs(bridge, exist_ok=True)\n"
              << "        rid=str(uuid.uuid4())\n"
              << "        response=os.path.join(bridge, rid + '.response.json')\n"
              << "        request=os.path.join(bridge, rid + '.request.json')\n"
              << "        with open(request, 'w', encoding='utf-8') as fp: json.dump({'type':'input_panel','caption':str(caption),'initial_text':str(initial_text or ''),'response':response}, fp)\n"
              << "        deadline=time.time()+120\n"
              << "        while time.time() < deadline:\n"
              << "            if os.path.exists(response):\n"
              << "                try:\n"
              << "                    with open(response, 'r', encoding='utf-8') as fp: data=json.load(fp)\n"
              << "                    try: os.remove(response)\n"
              << "                    except Exception: pass\n"
              << "                except Exception: data={'accepted':False,'text':''}\n"
              << "                if data.get('accepted'):\n"
              << "                    text=str(data.get('text',''))\n"
              << "                    if on_change: on_change(text)\n"
              << "                    on_done(text)\n"
              << "                elif on_cancel:\n"
              << "                    on_cancel()\n"
              << "                return None\n"
              << "            time.sleep(0.05)\n"
              << "        if on_cancel: on_cancel()\n"
              << "        return None\n"
              << "    def run_command(self, name, args=None):\n"
              << "        args=args or {}; cls=_find_command_class(name)\n"
              << "        if not cls: print('[window-command]', name, args); return\n"
              << "        plugin_mod=__import__('sub'+'lime_plugin')\n"
              << "        base=getattr(plugin_mod, 'WindowCommand', None)\n"
              << "        inst=cls(self) if base and issubclass(cls, base) else cls(None)\n"
              << "        try: inst.run(**args)\n"
              << "        except TypeError: inst.run()\n"
              << "    def new_file(self, *args, **kwargs):\n"
              << "        v=_ensure_view_api(View(self)); self._views.append(v); self._active_view=v; return v\n"
              << "    def open_file(self, path, *args, **kwargs):\n"
              << "        v=_ensure_view_api(View(self)); self._views.append(v); self._active_view=v; v.title=os.path.basename(str(path)); v._settings.set('file_name', str(path)); v._emit(True); return v\n"
              << "    def create_output_panel(self, name, *args, **kwargs):\n"
              << "        key=str(name)\n"
              << "        panel=getattr(self, '_output_panels', {}).get(key)\n"
              << "        if panel and panel.is_valid(): return _ensure_view_api(panel)\n"
              << "        v=_ensure_view_api(View(self)); v.set_name(key); v.set_scratch(True); v.settings().set('output_panel', key); self._views.append(v); self._output_panels[key]=v; return v\n"
              << "    def find_output_panel(self, name):\n"
              << "        panel=getattr(self, '_output_panels', {}).get(str(name))\n"
              << "        return _ensure_view_api(panel) if panel and panel.is_valid() else None\n"
              << "    def destroy_output_panel(self, name):\n"
              << "        panel=getattr(self, '_output_panels', {}).pop(str(name), None)\n"
              << "        if panel: panel._valid=False; self._views=[v for v in self._views if v is not panel]\n"
              << "    def show_panel(self, panel='', toggle=False):\n"
              << "        name=str(panel).split('.', 1)[1] if str(panel).startswith('output.') else str(panel)\n"
              << "        view=self.find_output_panel(name)\n"
              << "        if view: self.focus_view(view)\n"
              << "    def views(self):\n"
              << "        self._views=[_ensure_view_api(v) for v in self._views if getattr(v, 'is_valid', lambda: True)()]\n"
              << "        return list(self._views)\n"
              << "    def active_view(self):\n"
              << "        self.views()\n"
              << "        return _ensure_view_api(self._active_view if getattr(self, '_active_view', None) and self._active_view.is_valid() else (self._views[-1] if self._views else None))\n"
              << "    def focus_view(self, view):\n"
              << "        view=_ensure_view_api(view)\n"
              << "        if not view or not view.is_valid(): return\n"
              << "        self._active_view=view; self._active_view._emit(True)\n"
              << "Window._views = []\n"
              << "Window._active_view = None\n"
              << "Window._output_panels = {}\n"
              << "_active_window = Window()\n"
              << "def active_window(): return _active_window\n"
              << "def windows(): return []\n"
              << "class CompletionItem:\n"
              << "    @staticmethod\n"
              << "    def snippet_completion(trigger, annotation='', completion='', completion_format=None, kind=None, details=''): return (trigger, completion)\n"
              << "class CompletionList:\n"
              << "    def __init__(self, completions=None, flags=0): self.completions=completions or []; self.flags=flags\n"
              << "class Region:\n"
              << "    def __init__(self, a=0, b=None): self.a=a; self.b=a if b is None else b\n"
              << "    def begin(self): return min(self.a,self.b)\n"
              << "    def end(self): return max(self.a,self.b)\n"
              << "    def size(self): return abs(self.b-self.a)\n"
              << "    def empty(self): return self.a==self.b\n"
              << "    def contains(self, point):\n"
              << "        if hasattr(point, 'begin'): return self.begin() <= point.begin() and point.end() <= self.end()\n"
              << "        return self.begin() <= int(point) <= self.end()\n";
        }
        {
            std::ofstream f(lib / (api + "_plugin.py"), std::ios::binary);
            if (!f) return false;
            f << "class ApplicationCommand: pass\n"
              << "class WindowCommand:\n"
              << "    def __init__(self, window=None): self.window=window\n"
              << "class TextCommand:\n"
              << "    def __init__(self, view=None): self.view=view\n"
              << "class EventListener: pass\n"
              << "class ViewEventListener:\n"
              << "    def __init__(self, view=None): self.view=view\n";
        }
        {
            std::ofstream f(lib / "moreno_plugin_bootstrap.py", std::ios::binary);
            if (!f) return false;
            f << "import importlib.util, os, sys, traceback\n"
              << "_log=os.environ.get('MORENO_CONSOLE_LOG','')\n"
              << "if _log:\n"
              << "    os.makedirs(os.path.dirname(_log), exist_ok=True)\n"
              << "    _fp=open(_log, 'a', encoding='utf-8', buffering=1)\n"
              << "    sys.stdout=_fp; sys.stderr=_fp\n"
              << "sys.path.insert(0, os.environ['MORENO_API_PATH'])\n"
              << "for p in os.environ.get('MORENO_PACKAGE_PATHS','').split(os.pathsep):\n"
              << "    if p and p not in sys.path: sys.path.insert(0,p)\n"
              << "for script in os.environ.get('MORENO_PLUGIN_SCRIPTS','').split(os.pathsep):\n"
              << "    if not script: continue\n"
              << "    name='moreno_package_' + str(abs(hash(script)))\n"
              << "    try:\n"
              << "        print('reloading plugin ' + os.path.basename(os.path.dirname(script)) + '.' + os.path.splitext(os.path.basename(script))[0])\n"
              << "        spec=importlib.util.spec_from_file_location(name, script)\n"
              << "        mod=importlib.util.module_from_spec(spec)\n"
              << "        sys.modules[name]=mod\n"
              << "        spec.loader.exec_module(mod)\n"
              << "        cb=getattr(mod,'plugin_loaded',None)\n"
              << "        if cb: cb()\n"
              << "        print('[plugin-loaded]', script)\n"
              << "    except Exception:\n"
              << "        traceback.print_exc()\n";
            f << "def _dispatch_window_command(cmd, args=None):\n"
              << "    import inspect\n"
              << "    args=args or {}\n"
              << "    for mod in list(sys.modules.values()):\n"
              << "        if not mod: continue\n"
              << "        for obj in vars(mod).values():\n"
              << "            if not inspect.isclass(obj): continue\n"
              << "            name=obj.__name__\n"
              << "            if name.endswith('Command'):\n"
              << "                raw=name[:-7]\n"
              << "                snake=''\n"
              << "                for i,ch in enumerate(raw):\n"
              << "                    if i and ch.isupper() and (raw[i-1].islower() or (i+1 < len(raw) and raw[i+1].islower())): snake += '_'\n"
              << "                    snake += ch.lower()\n"
              << "                if snake == cmd:\n"
              << "                    print('[plugin-command]', cmd)\n"
              << "                    api_mod=__import__('sub'+'lime')\n"
              << "                    plugin_mod=__import__('sub'+'lime_plugin')\n"
              << "                    base=getattr(plugin_mod, 'WindowCommand', None)\n"
              << "                    inst=obj(api_mod.active_window()) if base and issubclass(obj, base) else obj(None)\n"
              << "                    try: inst.run(**args)\n"
              << "                    except TypeError:\n"
              << "                        try: inst.run()\n"
              << "                        except TypeError: inst.run(None)\n"
              << "                    return True\n"
              << "    print('[plugin-command-missing]', cmd)\n"
              << "    return False\n"
              << "cmd=os.environ.get('MORENO_PLUGIN_COMMAND','')\n"
              << "if cmd:\n"
              << "    _dispatch_window_command(cmd)\n"
              << "    if os.environ.get('MORENO_PLUGIN_PERSISTENT','') != '1': raise SystemExit(0)\n"
              << "if os.environ.get('MORENO_PLUGIN_PERSISTENT','') == '1':\n"
              << "    import json, time\n"
              << "    bridge=os.environ.get('MORENO_BRIDGE_DIR','')\n"
              << "    seen=set()\n"
              << "    print('[plugin-host] persistent ready')\n"
              << "    while True:\n"
              << "        try:\n"
              << "            if bridge and os.path.isdir(bridge):\n"
              << "                for name in sorted(os.listdir(bridge)):\n"
              << "                    if not name.endswith('.request.json'): continue\n"
              << "                    path=os.path.join(bridge, name)\n"
              << "                    if path in seen: continue\n"
              << "                    seen.add(path)\n"
              << "                    try:\n"
              << "                        with open(path, 'r', encoding='utf-8') as fp: data=json.load(fp)\n"
              << "                    except FileNotFoundError:\n"
              << "                        continue\n"
              << "                    api_mod=__import__('sub'+'lime')\n"
              << "                    lifecycle=getattr(api_mod, '_handle_bridge_lifecycle_request', None)\n"
              << "                    if lifecycle and lifecycle(data): continue\n"
              << "                    if data.get('type') != 'plugin_command': continue\n"
              << "                    _dispatch_window_command(str(data.get('command','')), data.get('args') or {})\n"
              << "        except Exception:\n"
              << "            traceback.print_exc()\n"
              << "        time.sleep(0.05)\n";
        }
        return true;
    } catch (...) { return false; }
}

bool PluginHost::writeCommandRequest(const PluginHostPaths& paths, const std::string& commandName) {
    if (commandName.empty()) return false;
    try {
        fs::path bridge = fs::path(paths.dataDir) / "Local" / "PluginBridge";
        fs::create_directories(bridge);
        nlohmann::json j;
        j["type"] = "plugin_command";
        j["command"] = commandName;
        j["args"] = nlohmann::json::object();
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        fs::path request = bridge / (std::to_string(stamp) + "-command.request.json");
        std::ofstream f(request, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump();
        return true;
    } catch (...) {
        return false;
    }
}

void PluginHost::launchHost(const PluginHostPaths& paths, const std::string& commandName, bool persistent) {
    auto scripts = discoverScripts(paths.packagesDir);
    if (scripts.empty() || !writeBootstrapFiles(paths)) return;
    std::string scriptList, packageList;
    for (auto& script : scripts) {
        if (!scriptList.empty()) scriptList += ';';
        scriptList += script.path;
        fs::path packageDir = fs::path(script.path).parent_path();
        std::string packagePath = packageDir.string();
        if (packageList.find(packagePath) == std::string::npos) {
            if (!packageList.empty()) packageList += ';';
            packageList += packagePath;
        }
    }
    fs::path boot = fs::path(paths.libDir) / "python38" / "moreno_plugin_bootstrap.py";
#ifdef _WIN32
    fs::path log = fs::path(paths.dataDir) / "Local" / "Console.log";
    fs::path bridge = fs::path(paths.dataDir) / "Local" / "PluginBridge";
    fs::path host = hostExecutablePath(paths);
    fs::create_directories(log.parent_path());
    fs::create_directories(bridge);
    auto appendLog = [&](const std::string& line) {
        try {
            std::ofstream out(log, std::ios::binary | std::ios::app);
            out << line << "\n";
        } catch (...) {}
    };
    if (!fs::exists(host)) {
        appendLog("[plugin-host] missing bundled host: " + host.string());
        return;
    }
    if (persistent) stopPreviousPluginHost(paths, host, appendLog);
    SetEnvironmentVariableA("MORENO_API_PATH", (fs::path(paths.libDir) / "python38").string().c_str());
    SetEnvironmentVariableA("MORENO_PACKAGES_DIR", paths.packagesDir.c_str());
    SetEnvironmentVariableA("MORENO_PACKAGES_PATH", paths.packagesDir.c_str());
    SetEnvironmentVariableA("MORENO_INSTALLED_PACKAGES_PATH", paths.installedPackagesDir.c_str());
    SetEnvironmentVariableA("MORENO_CACHE_PATH", paths.cacheDir.c_str());
    SetEnvironmentVariableA("MORENO_PACKAGE_PATHS", packageList.c_str());
    SetEnvironmentVariableA("MORENO_PLUGIN_SCRIPTS", scriptList.c_str());
    SetEnvironmentVariableA("MORENO_PLUGIN_COMMAND", commandName.empty() ? nullptr : commandName.c_str());
    SetEnvironmentVariableA("MORENO_PLUGIN_PERSISTENT", persistent ? "1" : nullptr);
    SetEnvironmentVariableA("MORENO_CONSOLE_LOG", log.string().c_str());
    SetEnvironmentVariableA("MORENO_BRIDGE_DIR", bridge.string().c_str());
    std::string command = "\"" + host.string() + "\" \"" + boot.string() + "\"";
    STARTUPINFOA si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, paths.dataDir.c_str(), &si, &pi)) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 1500);
        DWORD exitCode = 1;
        if (wait == WAIT_TIMEOUT) {
            if (persistent) writePluginHostPid(paths, pi.dwProcessId);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            return;
        }
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (exitCode != 0) appendLog("[plugin-host] host exited with code " + std::to_string(exitCode));
    } else {
        appendLog("[plugin-host] failed to launch bundled host: " + std::to_string(GetLastError()));
    }
#endif
}

void PluginHost::start(const PluginHostPaths& paths) { launchHost(paths, "", true); }
void PluginHost::runCommand(const PluginHostPaths& paths, const std::string& commandName) {
    if (!writeCommandRequest(paths, commandName)) launchHost(paths, commandName, false);
}
