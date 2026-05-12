import { defineConfig, type Plugin } from "vite";
import { readdirSync, statSync, createReadStream } from "node:fs";
import { resolve, join } from "node:path";

// Local seed dump produced by mapdump.exe. Resolved relative to repo root.
const SEED_DIR = resolve(__dirname, "..", "seedgen", "out");

// Force a full page reload on any source change. The viewer has lots of
// module-level side effects (canvas listeners, ResizeObserver, RAF loop)
// that don't survive partial HMR cleanly, and a full reload is fast.
// Production builds strip every element tagged `class="… dev-only …"` so the
// JSON upload row and embedded-seed dropdown literally don't exist in the
// shipped HTML. (Run via `vite` for the dev workflow, which keeps them.)
function stripDevOnlyPlugin(): Plugin {
  return {
    name: "d2-strip-dev-only",
    apply: "build",
    transformIndexHtml(html) {
      return html.replace(
        /\s*<([a-zA-Z][a-zA-Z0-9]*)[^>]*\bclass="[^"]*\bdev-only\b[^"]*"[^>]*>[\s\S]*?<\/\1>/g,
        "",
      );
    },
  };
}

function fullReloadPlugin(): Plugin {
  return {
    name: "d2-full-reload",
    handleHotUpdate({ server }) {
      server.ws.send({ type: "full-reload" });
      return [];
    },
  };
}

function seedDirPlugin(): Plugin {
  return {
    name: "d2-seed-dir",
    configureServer(server) {
      server.middlewares.use("/api/seeds", (_req, res) => {
        try {
          const files = readdirSync(SEED_DIR)
            .filter((f) => f.endsWith(".json"))
            .sort();
          res.setHeader("Content-Type", "application/json");
          res.end(JSON.stringify({ dir: SEED_DIR, files }));
        } catch (err: any) {
          res.statusCode = 500;
          res.end(JSON.stringify({ error: String(err?.message ?? err) }));
        }
      });

      server.middlewares.use("/api/seed", (req, res) => {
        const url = new URL(req.url ?? "/", "http://x");
        const name = url.searchParams.get("name") ?? "";
        // Reject anything that escapes the seed directory.
        if (!/^seed_[0-9]+\.json$/.test(name)) {
          res.statusCode = 400;
          res.end("bad name");
          return;
        }
        const path = join(SEED_DIR, name);
        try {
          statSync(path);
        } catch {
          res.statusCode = 404;
          res.end("not found");
          return;
        }
        res.setHeader("Content-Type", "application/json");
        createReadStream(path).pipe(res);
      });
    },
  };
}

export default defineConfig({
  plugins: [stripDevOnlyPlugin(), fullReloadPlugin(), seedDirPlugin()],
  server: {
    port: 5173,
    fs: { allow: [resolve(__dirname, ".."), __dirname] },
    // /api/seed[s] are handled by the local plugin above for static seed dumps.
    // /api/render goes to the live mapserver — start it separately with
    //     ./mapserver --static-dir renderer/dist --addr :8080
    // (or run the docker image).
    proxy: {
      "/api/render": "http://localhost:8080",
      "/api/lookup": "http://localhost:8080",
      "/api/filter": "http://localhost:8080",
      // Forward every screenshot request to the Go server so both the
      // thumb and the full-size hover image are crop-processed identically
      // to production. Dev users need the mapserver running for the
      // screenshots to render (same expectation as /api/render).
      "/tower-screenshots": "http://localhost:8080",
    },
    // Polling is slightly heavier on CPU but reliably catches saves from
    // editors that use atomic-rename writes (common on Windows / WSL).
    watch: { usePolling: true, interval: 250 },
  },
  build: {
    target: "es2022",
    rollupOptions: {
      input: {
        main: resolve(__dirname, "index.html"),
        lookup: resolve(__dirname, "lookup.html"),
      },
    },
  },
});
