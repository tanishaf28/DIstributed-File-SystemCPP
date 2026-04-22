# C++ Program Guide

This workspace contains four standalone C++17 demos. Each file is a complete program with its own `main()` function, build command, and runtime flow.

## 1. `chat_server.cpp`

This program implements a TCP-based multi-user chat system.

How it works:
- `main()` chooses server mode or client mode from the command line.
- In server mode, `ChatServer::run()` creates a socket, binds to the requested port, and listens for incoming connections.
- Every client gets its own detached thread through `handleClient()`.
- Messages are parsed into `ChatMessage` objects and routed by type: join, chat, leave, list, or quit.
- Each channel stores membership and short message history so new users can see recent messages when they join.
- In client mode, `ChatClient::run()` connects to the server, sends the username and join message, then starts a background receive loop while the foreground thread reads typed input.

Build:
```bash
g++ -std=c++17 -pthread -o chat_server chat_server.cpp
```

Run server:
```bash
./chat_server server 9000
```

Run client:
```bash
./chat_server client 127.0.0.1 9000 alice general
```

## 2. `data_pipeline.cpp`

This program is a generic data-processing framework built around a `Dataset<T>` container.

How it works:
- `main()` runs several demo functions in sequence.
- `Dataset<T>` wraps a `std::vector<T>` and provides chainable transformations such as `map`, `filter`, `flatMap`, `groupBy`, `zip`, `take`, and `drop`.
- The demos show numeric pipelines, word-count style processing, parallel mapping with `std::async`, function composition, and CSV-style record processing.
- Each demo prints the intermediate and final results so you can trace the pipeline from input to output.

Build:
```bash
g++ -std=c++17 -pthread -O2 -o data_pipeline data_pipeline.cpp
```

Run:
```bash
./data_pipeline
```

## 3. `mini_dropbox.cpp`

This program is a distributed file storage demo with versioning.

How it works:
- `main()` selects one of four modes: `server`, `upload`, `download`, or `list`.
- In your example, `./mini_dropbox server 8080 disk ./storage` starts the server with the disk-backed engine and stores versions under `./storage`.
- `StorageFactory` hides which storage engine is created so the server does not need to know whether it is using disk or memory.
- `FileServer::run()` listens for connections and spawns a thread per client.
- Each request is handled by `RequestHandler`, which decodes the message, calls the storage engine, and sends a response.
- `LocalDiskEngine` writes each version as a separate file and records the latest version number in a metadata file, while `InMemoryEngine` keeps versions in RAM for demos or tests.
- The client side uses `FileClient` to connect once per command and exchange a single request/response pair.
- `upload` stores a new version, `list` prints every stored version, and `download ... 2` fetches version 2 of the named file.

Build:
```bash
g++ -std=c++17 -pthread -o mini_dropbox mini_dropbox.cpp
```

Run server:
```bash
./mini_dropbox server 8080
```

Upload a file:
```bash
./mini_dropbox upload 127.0.0.1 8080 myfile.txt
```

Download a version:
```bash
./mini_dropbox download 127.0.0.1 8080 myfile.txt 1
```

List versions:
```bash
./mini_dropbox list 127.0.0.1 8080 myfile.txt
```

## 4. `plugin_editor.cpp`

This program is a plugin-based text editor simulation.

How it works:
- `main()` creates an `Editor` facade and loads several plugins.
- `EditorContext` stores the shared buffer, cursor, file path, undo history, and event buses.
- Plugins subscribe to events such as text changes and file opens, which lets them react without being tightly coupled to the editor.
- `SyntaxHighlightPlugin`, `AutoSavePlugin`, `ThemePlugin`, and `WordCountPlugin` each demonstrate a different observer-style behavior.
- The demo then simulates typing, saving, and undoing so the event flow is visible in the terminal.

Build:
```bash
g++ -std=c++17 -o plugin_editor plugin_editor.cpp
```

Run:
```bash
./plugin_editor
```

## Quick mental model

- `chat_server.cpp` is about live network messaging.
- `data_pipeline.cpp` is about transforming collections through a fluent API.
- `mini_dropbox.cpp` is about file versioning over TCP.
- `plugin_editor.cpp` is about event-driven extension points inside an editor.

If you want, I can also turn this into a `README.md` style overview or add more line-by-line comments inside specific functions.
