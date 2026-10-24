import { build } from "esbuild";
import { readFile, writeFile, mkdir } from "node:fs/promises";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
const root = dirname(fileURLToPath(import.meta.url));
const out = resolve(process.argv[2] || "");
if (!process.argv[2]) throw Error("Usage: node build.mjs <new-output-directory>");
await mkdir(out, { recursive: false });
const hash = bytes => createHash("sha256").update(bytes).digest("hex");
const result = await build({ absWorkingDir: root, entryPoints: ["reader.js"], bundle: true,
    format: "iife", platform: "browser", target: "chrome130", write: false,
    minify: false, legalComments: "eof", metafile: true });
const bytes = result.outputFiles[0].contents;
await writeFile(join(out, "reader.js"), bytes);
const lock = await readFile(join(root, "package-lock.json"));
const pkg = JSON.parse(await readFile(join(root, "package.json"), "utf8"));
const sourceHashes = {};
for (const path of Object.keys(result.metafile.inputs).filter(path => !path.startsWith("node_modules/")))
    sourceHashes[path] = hash(await readFile(join(root, path)));
await writeFile(join(out, "bundle.json"), JSON.stringify({ schemaVersion: 1,
    packageLockSha256: hash(lock), sourceSha256: hash(await readFile(join(root, "reader.js"))),
    dependencies: pkg.dependencies, overrides: pkg.overrides, sourceHashes,
    files: [{ path: "reader.js", sha256: hash(bytes), bytes: bytes.length }]
}, null, 2));
await writeFile(join(out, "metafile.json"), JSON.stringify(result.metafile, null, 2));
// Retain complete notices for all resolved packages, including transitive ones.
const installed = JSON.parse(lock).packages;
let notices = "EPUB P0 developer bundle: third-party notices\n\n";
for (const [path, info] of Object.entries(installed)) {
    if (!path || info.optional) continue;
    notices += `${path} ${info.version} (${info.license || "see package"})\n`;
    for (const name of ["LICENSE", "LICENSE.md", "LICENSE.txt", "LICENSE-MIT", "license"]) {
        try { notices += await readFile(join(root, path, name), "utf8"); notices += "\n"; break; } catch {}
    }
    notices += "\n";
}
await writeFile(join(out, "THIRD-PARTY-NOTICES.txt"), notices);
console.log(JSON.stringify({ output: out, bundleBytes: bytes.length, sha256: hash(bytes) }));
