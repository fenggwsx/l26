// l26web - a standalone web visualization demo server.
//
// It embeds the static assets under web/ (index.html / app.js / style.css plus
// the emscripten outputs l26.js / l26.wasm) into a single binary via //go:embed
// and serves them over the standard-library net/http. net/http spawns one
// goroutine per connection, so a browser fetching several assets concurrently
// never blocks.
//
// This is a demo shell kept separate from the compiler proper, l26c (written in
// C) - demonstration and use stay independent. The embedded l26.js / l26.wasm
// are still emscripten outputs and must be produced first via `make wasm`.
//
// Usage:
//
//	l26web                 listen on 127.0.0.1:8080
//	l26web -port 9000      pick a port
//	l26web -addr 0.0.0.0   expose externally (local-only by default)
package main

import (
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"mime"
	"net"
	"net/http"
	"os"
)

//go:embed index.html app.js style.css l26.js l26.wasm
var assets embed.FS

func main() {
	port := flag.Int("port", 8080, "listen port")
	addr := flag.String("addr", "127.0.0.1", "listen address (set to 0.0.0.0 to expose externally)")
	flag.Parse()

	// Some systems' MIME registries do not map .wasm to application/wasm, which
	// makes WebAssembly.instantiateStreaming fail on a MIME mismatch. Register
	// it explicitly as a fallback.
	if err := mime.AddExtensionType(".wasm", "application/wasm"); err != nil {
		log.Printf("warning: could not register .wasm MIME type: %v", err)
	}

	// The embed.FS root is the web/ content itself; serve it directly.
	root, err := fs.Sub(assets, ".")
	if err != nil {
		log.Fatalf("l26web: could not open embedded assets: %v", err)
	}
	mux := http.NewServeMux()
	mux.Handle("/", http.FileServer(http.FS(root)))

	hostport := net.JoinHostPort(*addr, fmt.Sprintf("%d", *port))

	// Bind first so port-in-use and similar errors surface clearly right away.
	ln, err := net.Listen("tcp", hostport)
	if err != nil {
		fmt.Fprintf(os.Stderr, "l26web: could not listen on %s: %v\n", hostport, err)
		os.Exit(1)
	}

	fmt.Printf("L26 web demo started: http://%s\n", hostport)
	fmt.Println("Press Ctrl+C to stop.")

	if err := http.Serve(ln, mux); err != nil {
		fmt.Fprintf(os.Stderr, "l26web: server exited: %v\n", err)
		os.Exit(1)
	}
}
