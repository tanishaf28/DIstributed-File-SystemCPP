/**
 * ============================================================
 *  PLUGIN-BASED TEXT EDITOR — VS Code–like Architecture
 *  C++17 Reference Implementation
 * ============================================================
 *
 * BUILD:
 *   g++ -std=c++17 -o plugin_editor plugin_editor.cpp
 *
 * RUN:
 *   ./plugin_editor
 *
 * ============================================================
 *  CONCEPTS COVERED (tagged in comments)
 * ============================================================
 *  [ABSTRACT]     - Pure abstract Plugin base class
 *  [POLYMORPHISM] - Runtime dispatch through Plugin*
 *  [OBSERVER]     - Event system with publisher/subscriber
 *  [SMART_PTR]    - unique_ptr for plugin ownership
 *  [STL]          - vector, map, string, algorithm
 *  [RAII]         - EditorContext resource management
 *  [LAMBDA]       - Callbacks for event handlers
 *  [MOVE]         - Move semantics for buffer operations
 *  [TEMPLATE]     - Generic EventBus<T>
 *  [SERIALIZE]    - File save/load with state
 * ============================================================
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <ctime>
#include <optional>

// ══════════════════════════════════════════════════════════════
//  SECTION 1: EVENT SYSTEM [OBSERVER] [TEMPLATE] [LAMBDA]
// ══════════════════════════════════════════════════════════════

/**
 * [TEMPLATE] EventBus<T> — generic publish/subscribe bus.
 * Any subsystem can subscribe to events of type T.
 * Plugins use this to react to editor state changes.
 */
template<typename T>
class EventBus {                               // [TEMPLATE]
public:
    using Handler = std::function<void(const T&)>;  // [LAMBDA] handler type

    // Subscribe: returns a handler ID for later unsubscription
    int subscribe(Handler h) {
        int id = next_id_++;
        handlers_[id] = std::move(h);          // [MOVE]
        return id;
    }

    void unsubscribe(int id) {
        handlers_.erase(id);
    }

    // Publish: notify all subscribers
    void publish(const T& event) {
        for (auto& [id, h] : handlers_) h(event);
    }

private:
    std::map<int, Handler> handlers_;
    int next_id_ = 0;
};

// ─── Event types ──────────────────────────────────────────────

struct TextChangedEvent {
    std::string new_content;
    int         cursor_line;
    int         cursor_col;
};

struct FileSavedEvent {
    std::string filepath;
    size_t      bytes_written;
};

struct FileOpenedEvent {
    std::string filepath;
};

struct CursorMovedEvent {
    int line, col;
};


// ══════════════════════════════════════════════════════════════
//  SECTION 2: EDITOR CONTEXT [RAII]
// ══════════════════════════════════════════════════════════════

/**
 * [RAII] EditorContext — owns the editor's state.
 * Passed by reference to all plugins so they share the same buffer.
 * Holds the event buses; cleanup is automatic.
 */
struct EditorContext {
    std::string              buffer;       // Current file contents
    std::string              filepath;     // Current file path
    int                      cursor_line = 0;
    int                      cursor_col  = 0;
    bool                     modified    = false;
    std::string              theme       = "default";
    std::vector<std::string> undo_stack;  // [STL]

    // [OBSERVER] Event buses — plugins subscribe to these
    EventBus<TextChangedEvent> onTextChanged;
    EventBus<FileSavedEvent>   onFileSaved;
    EventBus<FileOpenedEvent>  onFileOpened;
    EventBus<CursorMovedEvent> onCursorMoved;

    // Set buffer and fire event
    void setText(const std::string& text) {
        undo_stack.push_back(buffer);          // save undo state
        buffer   = text;
        modified = true;
        onTextChanged.publish({buffer, cursor_line, cursor_col});
    }

    // [MOVE] Set buffer with move (avoids copy for large text)
    void setTextMove(std::string&& text) {     // [MOVE]
        undo_stack.push_back(std::move(buffer));
        buffer   = std::move(text);            // [MOVE]
        modified = true;
        onTextChanged.publish({buffer, cursor_line, cursor_col});
    }

    void moveCursor(int line, int col) {
        cursor_line = line; cursor_col = col;
        onCursorMoved.publish({line, col});
    }

    bool undo() {
        if (undo_stack.empty()) return false;
        buffer = std::move(undo_stack.back()); // [MOVE]
        undo_stack.pop_back();
        modified = true;
        return true;
    }

    // [SERIALIZE] Save buffer to file
    bool saveToFile(const std::string& path = "") {
        std::string target = path.empty() ? filepath : path;
        if (target.empty()) return false;
        std::ofstream f(target);
        if (!f) return false;
        f << buffer;
        modified = false;
        filepath = target;
        onFileSaved.publish({target, buffer.size()});
        return true;
    }

    // [SERIALIZE] Load file into buffer
    bool loadFromFile(const std::string& path) {
        std::ifstream f(path);
        if (!f) return false;
        buffer = std::string(
            std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>());
        filepath = path;
        modified = false;
        onFileOpened.publish({path});
        return true;
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 3: PLUGIN INTERFACE [ABSTRACT] [POLYMORPHISM]
// ══════════════════════════════════════════════════════════════

/**
 * [ABSTRACT] Plugin — pure abstract base class for all plugins.
 * Every feature of the editor (highlight, autosave, themes)
 * is a Plugin. Demonstrates the Open/Closed Principle.
 */
class Plugin {
public:
    virtual ~Plugin() = default;

    // Called once when plugin is loaded into the editor
    virtual void onLoad(EditorContext& ctx) = 0;

    // Called once when plugin is unloaded
    virtual void onUnload(EditorContext& ctx) {}

    // Human-readable name
    virtual std::string name() const = 0;

    // Version string
    virtual std::string version() const { return "1.0.0"; }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 4: CONCRETE PLUGINS [POLYMORPHISM] [OBSERVER]
// ══════════════════════════════════════════════════════════════

/**
 * SyntaxHighlightPlugin — scans buffer for C++ keywords and
 * annotates them (simplified: prints annotated lines to stdout).
 * [OBSERVER] Subscribes to TextChangedEvent.
 */
class SyntaxHighlightPlugin : public Plugin {  // [POLYMORPHISM]
    int handler_id_ = -1;
    // [STL] keyword list
    static const std::vector<std::string>& keywords() {
        static std::vector<std::string> kws = {
            "int","float","double","char","bool","void","return",
            "if","else","for","while","class","struct","public",
            "private","protected","const","auto","template","typename",
            "namespace","using","include","define","virtual","override"
        };
        return kws;
    }

    std::string highlight(const std::string& line) {
        std::string result;
        std::istringstream ss(line);
        std::string word;
        bool first = true;
        // Simplified: mark keywords with [KW:word]
        // Real editor would produce coloured terminal output
        std::string annotated = line;
        for (const auto& kw : keywords()) {
            size_t pos = 0;
            while ((pos = annotated.find(kw, pos)) != std::string::npos) {
                // Simple whole-word check
                bool left_ok  = (pos == 0 || !std::isalnum(annotated[pos-1]));
                bool right_ok = (pos + kw.size() >= annotated.size()
                                 || !std::isalnum(annotated[pos + kw.size()]));
                if (left_ok && right_ok) {
                    std::string tag = "[KW:" + kw + "]";
                    annotated.replace(pos, kw.size(), tag);
                    pos += tag.size();
                } else {
                    pos += kw.size();
                }
            }
        }
        return annotated;
    }

public:
    void onLoad(EditorContext& ctx) override {
        // [OBSERVER] [LAMBDA] Subscribe to text changes
        handler_id_ = ctx.onTextChanged.subscribe([this](const TextChangedEvent& e) {
            std::cout << "[SyntaxHighlight] Re-highlighting " 
                      << std::count(e.new_content.begin(), e.new_content.end(), '\n') + 1
                      << " lines\n";
            // In a real editor, this would update the syntax tree
        });
        std::cout << "[Plugin] SyntaxHighlight loaded\n";
    }

    void onUnload(EditorContext& ctx) override {
        ctx.onTextChanged.unsubscribe(handler_id_);
    }

    std::string name()    const override { return "SyntaxHighlight"; }
    std::string version() const override { return "2.1.0"; }

    // Public utility: highlight a single line (usable by tests)
    std::string highlightLine(const std::string& line) { return highlight(line); }
};

/**
 * AutoSavePlugin — saves the buffer to disk periodically.
 * [OBSERVER] Subscribes to TextChangedEvent to track dirtiness.
 * Uses a simple counter to simulate a timer (real impl: std::thread + sleep).
 */
class AutoSavePlugin : public Plugin {
    int  handler_id_  = -1;
    int  change_count_= 0;
    int  save_every_  = 5;   // save every N changes

public:
    explicit AutoSavePlugin(int save_every = 5) : save_every_(save_every) {}

    void onLoad(EditorContext& ctx) override {
        handler_id_ = ctx.onTextChanged.subscribe([this, &ctx](const TextChangedEvent&) {
            ++change_count_;
            if (change_count_ % save_every_ == 0) {
                if (ctx.saveToFile()) {
                    std::cout << "[AutoSave] Saved " << ctx.filepath << "\n";
                }
            }
        });
        std::cout << "[Plugin] AutoSave loaded (every " << save_every_ << " changes)\n";
    }

    void onUnload(EditorContext& ctx) override {
        ctx.onTextChanged.unsubscribe(handler_id_);
    }

    std::string name() const override { return "AutoSave"; }
};

/**
 * ThemePlugin — changes visual theme by modifying ctx.theme.
 * [OBSERVER] Subscribes to FileOpenedEvent to pick a theme based on extension.
 */
class ThemePlugin : public Plugin {
    int handler_id_ = -1;

    // [STL] extension → theme map
    std::map<std::string, std::string> ext_themes_ = {
        {".cpp", "cpp-dark"}, {".h", "cpp-dark"},
        {".py",  "python-light"}, {".md", "markdown"}
    };

    std::string extensionOf(const std::string& path) {
        auto pos = path.rfind('.');
        return pos != std::string::npos ? path.substr(pos) : "";
    }

public:
    void onLoad(EditorContext& ctx) override {
        handler_id_ = ctx.onFileOpened.subscribe([this, &ctx](const FileOpenedEvent& e) {
            auto ext = extensionOf(e.filepath);
            auto it  = ext_themes_.find(ext);
            if (it != ext_themes_.end()) {
                ctx.theme = it->second;
                std::cout << "[Theme] Applied theme: " << ctx.theme << "\n";
            }
        });
        std::cout << "[Plugin] Theme loaded\n";
    }

    void onUnload(EditorContext& ctx) override {
        ctx.onFileOpened.unsubscribe(handler_id_);
    }

    std::string name() const override { return "Theme"; }
};

/**
 * WordCountPlugin — shows live word/line/char count.
 * [OBSERVER] Listens to TextChangedEvent.
 */
class WordCountPlugin : public Plugin {
    int handler_id_ = -1;

    struct Stats { size_t chars, words, lines; };

    Stats compute(const std::string& text) {
        Stats s{};
        s.chars = text.size();
        s.lines = std::count(text.begin(), text.end(), '\n') + 1;
        std::istringstream ss(text);
        std::string word;
        while (ss >> word) ++s.words;
        return s;
    }

public:
    void onLoad(EditorContext& ctx) override {
        handler_id_ = ctx.onTextChanged.subscribe([this](const TextChangedEvent& e) {
            auto st = compute(e.new_content);
            std::cout << "[WordCount] " << st.lines << " lines | "
                      << st.words << " words | " << st.chars << " chars\n";
        });
        std::cout << "[Plugin] WordCount loaded\n";
    }

    void onUnload(EditorContext& ctx) override {
        ctx.onTextChanged.unsubscribe(handler_id_);
    }

    std::string name() const override { return "WordCount"; }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 5: PLUGIN MANAGER [SMART_PTR] [STL]
// ══════════════════════════════════════════════════════════════

/**
 * PluginManager — owns and lifecycle-manages all plugins.
 * [SMART_PTR] Stores plugins as unique_ptr — no manual delete.
 * [POLYMORPHISM] Iterates through Plugin* calling virtual methods.
 */
class PluginManager {
    // [SMART_PTR] unique_ptr vector — plugins owned here
    std::vector<std::unique_ptr<Plugin>> plugins_;
    EditorContext& ctx_;

public:
    explicit PluginManager(EditorContext& ctx) : ctx_(ctx) {}

    // [SMART_PTR] [MOVE] Takes ownership of plugin
    void load(std::unique_ptr<Plugin> plugin) {
        plugin->onLoad(ctx_);                      // [POLYMORPHISM]
        plugins_.push_back(std::move(plugin));     // [MOVE]
    }

    void unloadAll() {
        // [POLYMORPHISM] Each plugin's onUnload is called virtually
        for (auto& p : plugins_) p->onUnload(ctx_);
        plugins_.clear();
    }

    void unload(const std::string& name) {
        // [STL] find_if with lambda
        auto it = std::find_if(plugins_.begin(), plugins_.end(),
            [&](const std::unique_ptr<Plugin>& p) { return p->name() == name; });
        if (it != plugins_.end()) {
            (*it)->onUnload(ctx_);
            plugins_.erase(it);
        }
    }

    void listPlugins() const {
        std::cout << "\n=== Loaded Plugins ===\n";
        for (const auto& p : plugins_)
            std::cout << "  • " << p->name() << " v" << p->version() << "\n";
        std::cout << "======================\n\n";
    }

    ~PluginManager() { unloadAll(); }  // [RAII]
};


// ══════════════════════════════════════════════════════════════
//  SECTION 6: EDITOR — top-level facade
// ══════════════════════════════════════════════════════════════

class Editor {
    EditorContext ctx_;
    PluginManager mgr_;

public:
    Editor() : mgr_(ctx_) {}

    // Load a plugin (takes ownership)
    void loadPlugin(std::unique_ptr<Plugin> p) {
        mgr_.load(std::move(p));              // [MOVE]
    }

    void openFile(const std::string& path) {
        if (!ctx_.loadFromFile(path))
            std::cerr << "[Editor] Cannot open: " << path << "\n";
        else
            std::cout << "[Editor] Opened: " << path << "\n";
    }

    void typeText(const std::string& text) {
        ctx_.setText(ctx_.buffer + text);
    }

    void saveFile(const std::string& path = "") {
        ctx_.saveToFile(path);
    }

    void undo() {
        if (!ctx_.undo())
            std::cout << "[Editor] Nothing to undo\n";
        else
            std::cout << "[Editor] Undo successful\n";
    }

    void listPlugins() const { mgr_.listPlugins(); }

    const EditorContext& context() const { return ctx_; }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 7: DEMO MAIN
// ══════════════════════════════════════════════════════════════

// Runtime flow:
// 1. `main()` creates the Editor facade and loads four plugins.
// 2. Typing text updates the shared EditorContext, which publishes events to
//    every plugin through the EventBus observers.
// 3. Saving and undoing mutate the same buffer state, so the plugins react to
//    the exact operations a real editor would expose.

int main() {
    std::cout << "=== Plugin-Based Text Editor Demo ===\n\n";

    Editor editor;

    // Load plugins — each is a unique_ptr [SMART_PTR]
    editor.loadPlugin(std::make_unique<SyntaxHighlightPlugin>());
    editor.loadPlugin(std::make_unique<AutoSavePlugin>(3));
    editor.loadPlugin(std::make_unique<ThemePlugin>());
    editor.loadPlugin(std::make_unique<WordCountPlugin>());

    editor.listPlugins();

    // Simulate typing — all plugins react via Observer pattern [OBSERVER]
    std::cout << "--- Typing code ---\n";
    editor.typeText("#include <iostream>\n");
    editor.typeText("int main() {\n");
    editor.typeText("    return 0;\n");
    editor.typeText("}\n");

    // Save the file [SERIALIZE]
    std::cout << "\n--- Saving ---\n";
    editor.saveFile("/tmp/test.cpp");

    // Demonstrate undo [MOVE]
    std::cout << "\n--- Undo ---\n";
    editor.undo();
    editor.undo();

    std::cout << "\n=== Demo Complete ===\n";
    return 0;
}

/*
 * ============================================================
 *  EXTENSION IDEAS
 * ============================================================
 *  1. Runtime plugin loading via dlopen() / shared libraries
 *  2. Async event queue (producer/consumer with std::queue + mutex)
 *  3. LSP (Language Server Protocol) integration as a plugin
 *  4. Search/replace plugin using std::regex
 *  5. Diff plugin using Myers diff algorithm
 * ============================================================
 */
