// RHWP H0-01: pinned, offline diagnostic bundle. No product CMake integration.
import fs from 'node:fs/promises';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const ToolDirectory = path.dirname(fileURLToPath(import.meta.url));
const LockPath = path.join(ToolDirectory, 'source-lock.json');
const Lock = JSON.parse(await fs.readFile(LockPath, 'utf8'));
const [Command, SourceArgument, OutputArgument, EngineArgument] = process.argv.slice(2);
const NativeEditor = Command === 'build-editor';
const Source = path.resolve(SourceArgument || '_build/rhwp-h0/source');
const Studio = path.join(Source, 'rhwp-studio');
const Hash = Bytes => createHash('sha256').update(Bytes).digest('hex');
const CrLf = Text => Text.replace(/\r?\n/g, '\r\n');
const WriteJson = async (File, Value) => fs.writeFile(File, CrLf(JSON.stringify(Value, null, 2) + '\n'), 'utf8');
const Git = (...Args) => execFileSync('git', ['-C', Source, ...Args], { windowsHide: true });
const CleanPath = Relative => {
    if (!Relative || Relative.includes('\\') || Relative.startsWith('/') ||
        Relative.split('/').some(Part => Part === '..' || Part === '.'))
        throw Error('Unsafe manifest path: ' + Relative);
    return Relative;
};
async function VerifySource() {
    if (Git('rev-parse', 'HEAD').toString().trim() !== Lock.upstream.commit)
        throw Error('Unexpected source revision');
    if (Git('status', '--porcelain', '--untracked-files=no').toString().trim())
        throw Error('Tracked upstream files changed; use a separate reviewed build transform');
    for (const File of Lock.criticalFiles) {
        const Relative = CleanPath(File.path);
        const Blob = Git('show', 'HEAD:' + Relative);
        if (Hash(Blob) !== File.sha256 ||
            Git('hash-object', '--path=' + Relative, Relative).toString().trim() !== File.gitBlob)
            throw Error('Source hash mismatch: ' + Relative);
    }
}
async function Walk(Directory, Prefix = '') {
    const Files = [];
    for (const Entry of await fs.readdir(Directory, { withFileTypes: true })) {
        const Relative = Prefix + Entry.name;
        if (Entry.isSymbolicLink()) throw Error('Bundle must not contain symbolic links: ' + Relative);
        if (Entry.isDirectory()) Files.push(...await Walk(path.join(Directory, Entry.name), Relative + '/'));
        else if (Entry.isFile()) Files.push(Relative);
        else throw Error('Unsupported bundle entry: ' + Relative);
    }
    return Files.sort();
}
async function Manifest(Directory) {
    const Files = [];
    for (const Relative of (await Walk(Directory)).filter(File => File !== 'asset-manifest.json')) {
        const Bytes = await fs.readFile(path.join(Directory, Relative));
        Files.push({ path: Relative, bytes: Bytes.length, sha256: Hash(Bytes) });
    }
    return { schemaVersion: 1, sourceCommit: Lock.upstream.commit, releaseEligible: false,
        sourceLockSha256: Hash(await fs.readFile(LockPath)),
        builderSha256: Hash(await fs.readFile(fileURLToPath(import.meta.url))),
        wasmProfile: Lock.build.profile, files: Files };
}
async function VerifyBundle(Directory) {
    const Recorded = JSON.parse(await fs.readFile(path.join(Directory, 'asset-manifest.json'), 'utf8'));
    const Actual = await Manifest(Directory);
    if (JSON.stringify(Recorded) !== JSON.stringify(Actual))
        throw Error('Bundle manifest mismatch (missing, added, modified file or changed build inputs)');
    for (const File of Recorded.files) CleanPath(File.path);
    console.log(JSON.stringify({ status: 'PASS', files: Recorded.files.length,
        sourceCommit: Recorded.sourceCommit, releaseEligible: false }));
}
async function Build(Directory) {
    await VerifySource();
    if (process.versions.node !== Lock.toolchain.node)
        throw Error('Use pinned Node ' + Lock.toolchain.node);
    const Pkg = NativeEditor ? path.resolve(EngineArgument || '_build/rhwp-h2/engine') : path.join(Source, 'pkg');
    for (const File of ['rhwp.js', 'rhwp_bg.wasm', 'rhwp.d.ts', 'package.json']) await fs.access(path.join(Pkg, File));
    const Package = JSON.parse(await fs.readFile(path.join(Pkg, 'package.json'), 'utf8'));
    if (Package.version !== Lock.upstream.tag.replace(/^v/, '')) throw Error('Unexpected WASM package version');
    if (NativeEditor) {
        const EngineLock = JSON.parse(await fs.readFile(path.join(ToolDirectory, 'engine-lock.json'), 'utf8'));
        if (EngineLock.sourceCommit !== Lock.upstream.commit) throw Error('Engine/source mismatch');
        for (const [File, Pin] of Object.entries(EngineLock.files))
            if (Hash(await fs.readFile(path.join(Pkg, File))) !== Pin) throw Error('Engine pin mismatch: ' + File);
    } else {
    const Receipt = JSON.parse(await fs.readFile(path.join(Pkg, 'hc-build-receipt.json'), 'utf8'));
    if (Receipt.sourceCommit !== Lock.upstream.commit || Receipt.sourceLockSha256 !== Hash(await fs.readFile(LockPath)))
        throw Error('WASM receipt does not match the pinned source inputs');
    for (const File of Receipt.files) {
        if (Hash(await fs.readFile(path.join(Pkg, CleanPath(File.path)))) !== File.sha256)
            throw Error('WASM receipt hash mismatch: ' + File.path);
    }
    if (!['rhwp.js', 'rhwp_bg.wasm'].every(Name => Receipt.files.some(File => File.path === Name)))
        throw Error('WASM receipt lacks required files');
    }
    const Wasm = await fs.readFile(path.join(Pkg, 'rhwp_bg.wasm'));
    if (Wasm.subarray(0, 8).toString('hex') !== '0061736d01000000') throw Error('Invalid WASM header');
    // Never empty or replace an existing output directory.
    await fs.mkdir(Directory);
    const Public = path.join(Directory, '.public-input');
    await fs.mkdir(Public);
    for (const Name of ['favicon.ico', 'icons', 'images', 'theme-init.js', 'locale-init.js', 'print.html'])
        await fs.cp(path.join(Studio, 'public', Name), path.join(Public, Name), { recursive: true });
    await fs.cp(path.join(Source, 'assets/fonts'), path.join(Public, 'fonts'), { recursive: true });
    await fs.copyFile(path.join(Pkg, 'rhwp.js'), path.join(Public, 'rhwp.js'));
    await fs.copyFile(path.join(Pkg, 'rhwp_bg.wasm'), path.join(Public, 'rhwp_bg.wasm'));
    const Notices = path.join(Public, 'notices');
    await fs.mkdir(Notices);
    for (const [From, To] of [
        ['LICENSE', 'RHWP-LICENSE.txt'], ['THIRD_PARTY_LICENSES.md', 'UPSTREAM-THIRD-PARTY.txt'],
        ['ttfs/opensource/NotoSansKR-OFL.txt', 'NotoSansKR-OFL.txt']
    ]) await fs.copyFile(path.join(Source, From), path.join(Notices, To));
    await fs.copyFile(LockPath, path.join(Notices, 'source-lock.json'));
    if (NativeEditor) await fs.copyFile(path.join(ToolDirectory, 'engine-lock.json'), path.join(Notices, 'engine-lock.json'));
    else await fs.copyFile(path.join(Pkg, 'hc-build-receipt.json'), path.join(Notices, 'wasm-build-receipt.json'));
    const { build } = await import(pathToFileURL(path.join(Studio, 'node_modules/vite/dist/node/index.js')).href);
    let MainTransforms = 0, SettingsTransforms = 0, ProtocolTransforms = 0;
    const ReplaceOnce = (Code, Before, After) => {
        if (Code.split(Before).length !== 2) throw Error('Pinned transform no longer matches: ' + Before.slice(0, 90));
        return Code.replace(Before, After);
    };
    const Csp = "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self' data: blob:; connect-src 'self' blob:; worker-src 'none'; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'";
    await build({
        configFile: false, root: Studio, base: './', publicDir: Public,
        define: { __APP_VERSION__: JSON.stringify(Package.version + '-hc'),
            __RHWP_DISABLE_EXTERNAL_WEBFONTS__: 'true', __RHWP_HWPCTRL__: 'false', __RHWP_AI__: 'false' },
        resolve: { alias: { '@': path.join(Studio, 'src'), '@wasm': Pkg, '@hc/editor-adapter': path.resolve(ToolDirectory, '../../UniqueLibs/document/rhwp/editorAdapter.js'), '@hc/save-commands': path.resolve(ToolDirectory, '../../UniqueLibs/document/rhwp/hostSaveCommands.js'), '@hc/host-print': path.resolve(ToolDirectory, '../../UniqueLibs/document/rhwp/hostPrint.js') } },
        plugins: [{
            name: 'hc-h0-offline-profile', enforce: 'pre',
            transform(Code, Id) {
                const Normalized = Id.replaceAll('\\', '/').split('?')[0];
                if (Normalized === path.join(Studio, 'src/core/extension-settings.ts').replaceAll('\\', '/')) {
                    SettingsTransforms++;
                    return 'export async function loadExtensionViewerSettings() { return { disableExternalWebFonts: true }; }';
                }
                if (Normalized === path.join(Studio, 'src/embed/protocol.ts').replaceAll('\\', '/')) {
                    ProtocolTransforms++;
                    let Text = Code.replaceAll('\r\n', '\n');
                    for (const Capability of ['document-state-v1', 'selection-context-v1', 'document-agent-command-v1', 'target-navigation-v1', 'document-change-events-v1'])
                        Text = ReplaceOnce(Text, "  '" + Capability + "',\n", '');
                    return Text;
                }
                // The native host accepts HWP/HWPX; retain the worker-free offline policy.
                if (Normalized === path.join(Source, 'rhwp-shared/pdf/import-session.js').replaceAll('\\', '/')) {
                    return ReplaceOnce(Code, "new Worker(new URL('./import-worker.js', import.meta.url), { type: 'module' })",
                        "(() => { throw new Error('PDF import is unavailable in the native HWP host'); })()");
                }
                if (NativeEditor && Normalized === path.join(Studio,'src/core/wasm-bridge.ts').replaceAll('\\','/')) {
                    let Text = Code.replaceAll('\r\n','\n');
                    Text = ReplaceOnce(Text,'    const wasmModule = await init() as { memory?: WebAssembly.Memory };',
                        "    const HcInit=(globalThis as any).__hcRhwpStartupTrace?.begin();\n    const wasmModule = await init() as { memory?: WebAssembly.Memory };\n    (globalThis as any).__hcRhwpStartupTrace?.end('js.wasmInit',HcInit);");
                    Text = ReplaceOnce(Text,'      nextDoc = createDocument();',
                        "      const HcParse=(globalThis as any).__hcRhwpStartupTrace?.begin();\n      nextDoc = createDocument();\n      (globalThis as any).__hcRhwpStartupTrace?.end('js.parseLayout',HcParse);");
                    Text = ReplaceOnce(Text,'    let nextDoc: HwpDocument | null = null;',
                        "    const HcEditable=(globalThis as any).__hcRhwpStartupTrace?.begin();\n    let nextDoc: HwpDocument | null = null;");
                    Text = ReplaceOnce(Text,'    const previousDoc = this.doc;',
                        "      (globalThis as any).__hcRhwpStartupTrace?.end('js.editableSetup',HcEditable);\n      const previousDoc = this.doc;");
                    Text = ReplaceOnce(Text, '    return hasPdfImportCapability(HwpDocument);', '    return false;');
                    const InfoLine='    return this.getDocumentInfo();';
                    // Only the open method: other public callers keep their behavior.
                    const OpenAt=Text.indexOf('  loadDocument(data: Uint8Array'),InfoAt=Text.indexOf(InfoLine,OpenAt);
                    if(OpenAt<0||InfoAt<OpenAt)throw Error('document info trace anchor');
                    Text=Text.slice(0,InfoAt)+"    const HcInfo=(globalThis as any).__hcRhwpStartupTrace?.begin();\n    try {return this.getDocumentInfo();}finally{(globalThis as any).__hcRhwpStartupTrace?.end('js.documentInfo',HcInfo);}"+Text.slice(InfoAt+InfoLine.length);
                    for(const Signature of ['  renderPageToCanvas(', '  renderPageToCanvasFiltered(']) {
                        const Start=Text.indexOf(Signature);if(Start<0)throw Error('render trace signature');
                        const Body=Text.indexOf('): void {',Start)+'): void {'.length,End=Text.indexOf('\n  }',Body);
                        if(Body<10||End<Body)throw Error('render trace body');
                        Text=Text.slice(0,Body)+"\n    const HcRender=pageNum===0?(globalThis as any).__hcRhwpStartupTrace?.begin():undefined;\n    try {"+Text.slice(Body,End)+"\n    } finally {if(HcRender!==undefined)(globalThis as any).__hcRhwpStartupTrace?.end('js.page0Render',HcRender,true);}"+Text.slice(End);
                    }
                    return Text;
                }
                if (Normalized !== path.join(Studio, 'src/main.ts').replaceAll('\\', '/')) return null;
                MainTransforms++;
                let Text = Code.replaceAll('\r\n', '\n');
                Text = ReplaceOnce(Text, "import { DocumentAgentController } from '@/document-agent/controller';", '');
                Text = ReplaceOnce(Text, '    documentAgent = new DocumentAgentController({\n      wasm,\n      input: inputHandler,\n      eventBus,\n      isDirty: () => documentState.isDirty(),\n      render: () => canvasView!.refreshDocumentAgentMutation(),\n    });', '    documentAgent = null;');
                Text = ReplaceOnce(Text, "  const fileUrl = params.get('url');", '  const fileUrl: string | null = null; // HC: document bytes enter through the host.');
                if (NativeEditor) {
                    Text = ReplaceOnce(Text, 'const chromeMode = chromeModeRequest.mode;', "const chromeMode = 'embed' as const;");
                    // Keep only the two native-backed save surfaces. Never register the
                    // upstream browser writer, even during editor initialization.
                    Text = ReplaceOnce(Text, "registry.registerAll(\n  chromeMode === 'embed'\n    ? fileCommands.filter",
                        "import { InstallHostSaveCommands } from '@hc/save-commands';\nconst hostSaveCommands = InstallHostSaveCommands(registry, fileCommands, eventBus);\nregistry.registerAll(\n  chromeMode === 'embed'\n    ? fileCommands.filter");
                    Text = ReplaceOnce(Text, '[...EMBED_HIDDEN_FILE_COMMAND_IDS, ...EMBED_HIDDEN_EDIT_COMMAND_IDS]',
                        "[...EMBED_HIDDEN_FILE_COMMAND_IDS.filter(id => id !== 'file:save' && id !== 'file:save-as' && id !== 'file:print'), ...EMBED_HIDDEN_EDIT_COMMAND_IDS]");
                    Text = ReplaceOnce(Text,'const initPromise = initialize();',
                        "const HcStudioInit=(globalThis as any).__hcRhwpStartupTrace?.begin();\nconst initPromise = initialize();\nif((globalThis as any).__hcRhwpStartupTrace)void initPromise.then(()=>(globalThis as any).__hcRhwpStartupTrace.end('js.studioInitialize',HcStudioInit),()=>{});");
                    Text = ReplaceOnce(Text, 'documentState.installBeforeUnload(window);' , '// Native owner confirms close.');
                    Text = ReplaceOnce(Text, 'recoveryEnabled: settings.recoveryEnabled,', 'recoveryEnabled: false,');
                    Text = ReplaceOnce(Text, 'idleEnabled: settings.idleSaveEnabled,', 'idleEnabled: false,');
                    Text += `
import { ConnectEditor } from '@hc/editor-adapter';
import * as PrintEngine from '@wasm';
void initPromise.then(() => {
    new QWebChannel(qt.webChannelTransport, Channel => {
        ConnectEditor(Channel, {
            printEngine: PrintEngine,
            load: async (Bytes, Format) => {
                await loadBytes(Bytes, 'document.' + Format, null, undefined, {suppressDialogs:true});
                return {pageCount:wasm.pageCount};
            },
            firstFrame: async () => {
                for(let Attempt=0;Attempt<600;++Attempt) {
                    await new Promise(Resolve=>requestAnimationFrame(()=>requestAnimationFrame(Resolve)));
                    const Canvas=document.querySelector('canvas.document-page-canvas');
                    if(Canvas?.width>0 && Canvas?.height>0) return {width:Canvas.width,height:Canvas.height};
                }
                throw Error('editor-first-frame');
            },
            setEnabled: Enabled => { document.body.inert=!Enabled; hostSaveCommands.setEnabled(Enabled); },
            setSaving: Saving => hostSaveCommands.setSaving(Saving),
            onChanged: Listener => { eventBus.on('document-changed',Listener); eventBus.on('document-mutated',Listener); },
            onAction: Listener => {
                hostSaveCommands.onAction(Listener);
                // Preserve host save shortcuts even when a ribbon field has focus.
                document.addEventListener('keydown', Event => {
                    if (!Event.isComposing && Event.ctrlKey && !Event.altKey && (Event.key.toLowerCase()==='s' || (!Event.shiftKey && Event.key.toLowerCase()==='p'))) {
                        Event.preventDefault(); Event.stopImmediatePropagation();
                        dispatcher.dispatch(Event.key.toLowerCase()==='p' ? 'file:print' : Event.shiftKey ? 'file:save-as' : 'file:save');
                    }
                }, true);
            },
            export: Format => Format==='hwp' ? wasm.exportHwpWithReport() : wasm.exportHwpxWithReport(),
            saved: () => completeHostSave(),
        }).catch(() => { document.body.inert=true; });
    });
});
`;
                }
                return Text;
            },
            transformIndexHtml: { order: 'pre', handler(Html) {
                return Html.replace('<head>', '<head>\n<meta http-equiv="Content-Security-Policy" content="' + Csp + '">');
            } }
        }],
        build: { outDir: path.join(Directory, 'site'), emptyOutDir: false,
            sourcemap: false, target: 'es2022', assetsInlineLimit: 0 }
    });
    if (MainTransforms !== 1 || SettingsTransforms !== 1 || ProtocolTransforms !== 1) throw Error('Offline transforms were not applied exactly once');
    // Vite consumes only the curated public tree: Windows symlink placeholder files are excluded.
    const Site = path.join(Directory, 'site');
    for (const Relative of await Walk(Site)) {
        if (/\.(html|js|css|json|txt|md)$/i.test(Relative)) {
            const File = path.join(Site, Relative);
            await fs.writeFile(File, CrLf(await fs.readFile(File, 'utf8')), 'utf8');
        }
    }
    await WriteJson(path.join(Site, 'asset-manifest.json'), await Manifest(Site));
    await VerifyBundle(Site);
    console.log('Diagnostic bundle: ' + Site);
}
try {
    if (Command === 'verify-source') { await VerifySource(); console.log('Pinned source: PASS'); }
    else if (Command === 'build' || NativeEditor) {
        if (!OutputArgument) throw Error('build requires a new output directory');
        await Build(path.resolve(OutputArgument));
    } else if (Command === 'verify-bundle') {
        await VerifyBundle(path.resolve(SourceArgument));
    } else throw Error('Usage: node build-bundle.mjs verify-source <source> | build <source> <new-output> | verify-bundle <site>');
} catch (ErrorObject) {
    console.error(ErrorObject.stack || ErrorObject.message);
    process.exitCode = 1;
}
