// Assemble a sealed diagnostic candidate. This tool never grants release approval.
import fs from 'node:fs/promises';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
const ExecFile = promisify(execFile);
const Hash = Bytes => createHash('sha256').update(Bytes).digest('hex');
const Json = Value => (JSON.stringify(Value, null, 2) + '\n').replace(/\r?\n/g, '\r\n');
const Safe = Name => typeof Name === 'string' && Name.length < 240 && Name.split('/').length <= 32
    && Name.split('/').every(Part => Part && !/[\\:*?"<>|\x00-\x1f]/.test(Part)
        && !/[. ]$/.test(Part) && !/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(Part));
async function Read(Root, Name) {
    if (!Safe(Name)) throw Error('Unsafe path: ' + Name);
    let Current = Root;
    for (const Part of Name.split('/')) {
        Current = path.join(Current, Part);
        if ((await fs.lstat(Current)).isSymbolicLink()) throw Error('Linked input: ' + Name);
    }
    const Stat = await fs.stat(Current);
    if (!Stat.isFile() || Stat.size > 512 * 1024 * 1024) throw Error('Invalid input size: ' + Name);
    return fs.readFile(Current);
}
// The runtime root is the product ROOT: Qt DLLs beside HyperEditorWeb.exe, plugins and WebEngine data
// under Libs/ (found through the qt.conf embedded in HyperEditorWeb.exe), bundle under Libs/bundle.
const Allowed = /^(HyperEditorWeb\.exe|QtWebEngineProcess\.exe|Qt6(?!Test)[A-Za-z0-9]+\.dll|(?:vcruntime|msvcp|concrt)[A-Za-z0-9_.]*\.dll|Libs\/(?:platforms|imageformats|iconengines|styles|generic|networkinformation|tls|sqldrivers|printsupport)\/[A-Za-z0-9_-]+\.dll|Libs\/resources\/(?:icudtl\.dat|qtwebengine_[a-z0-9_]+\.pak|v8_context_snapshot\.bin)|Libs\/translations\/qtwebengine_locales\/[A-Za-z0-9_-]+\.pak)$/;
const BundlePrefix = 'Libs/bundle/';
export async function BuildCandidate(Runtime, Bundle, Output, Storage = 'files') {
    if (!['files', 'qt-resource'].includes(Storage)) throw Error('Invalid bundle storage');
    Runtime = await fs.realpath(Runtime); Bundle = await fs.realpath(Bundle); Output = path.resolve(Output);
    if ([Runtime, Bundle].some(Root => Output.toLowerCase() === Root.toLowerCase() || Output.toLowerCase().startsWith(Root.toLowerCase() + path.sep)))
        throw Error('Output must be separate from inputs');
    const Lock = JSON.parse(await fs.readFile(new URL('engine-lock.json', import.meta.url), 'utf8'));
    const ManifestBytes = await Read(Bundle, 'asset-manifest.json');
    const Manifest = JSON.parse(ManifestBytes);
    if (Manifest.schemaVersion !== 1 || Manifest.sourceCommit !== Lock.sourceCommit || !['native-read-only','native-editor'].includes(Manifest.profile)
        || !Array.isArray(Manifest.files) || Manifest.files.length > 512 || Manifest.capabilities?.view !== true
        || ['edit','save'].some(Key => Manifest.capabilities?.[Key] !== (Manifest.profile === 'native-editor'))
        || (typeof Manifest.capabilities?.print !== 'boolean' || Manifest.capabilities?.recovery !== false)) throw Error('Invalid read-only bundle');
    if (Manifest.profile === 'native-editor' && !['editor/index.html','editor/policy.json'].every(Name=>Manifest.files.some(Entry=>Entry.path===Name))) throw Error('Missing editor resources');
    const Entries = new Map();
    const Seen = new Set();
    async function Add(Root, Name, Target, Pin, Size) {
        if (!Safe(Target) || Seen.has(Target.toLowerCase())) throw Error('Duplicate/unsafe path: ' + Target);
        const Bytes = await Read(Root, Name);
        const Sha = Hash(Bytes);
        if (Pin !== undefined && (!/^[0-9a-f]{64}$/.test(Pin) || Sha !== Pin || Bytes.length !== Size)) throw Error('Bundle hash/size mismatch: ' + Name);
        Seen.add(Target.toLowerCase()); Entries.set(Target, {root:Root, name:Name, bytes:Bytes.length, sha256:Sha});
    }
    for (const Entry of Manifest.files) {
        if (!Entry || typeof Entry.path !== 'string' || typeof Entry.sha256 !== 'string'
            || !/^[0-9a-f]{64}$/.test(Entry.sha256) || !Number.isSafeInteger(Entry.bytes) || Entry.bytes < 0) throw Error('Invalid bundle entry');
        await Add(Bundle, Entry.path, BundlePrefix + Entry.path, Entry.sha256, Entry.bytes);
    }
    for (const [Name, Pin] of Object.entries(Lock.files)) if (Entries.get(BundlePrefix + Name)?.sha256 !== Pin) throw Error('Engine pin mismatch: ' + Name);
    for (const Name of ['index.html','viewer.js','viewer.css','readOnlyAdapter.js','notices/RHWP-LICENSE.txt','notices/UPSTREAM-THIRD-PARTY.txt','notices/engine-lock.json'])
        if (!Entries.has(BundlePrefix + Name)) throw Error('Required bundle input: ' + Name);
    await Add(Bundle, 'asset-manifest.json', BundlePrefix + 'asset-manifest.json', Hash(ManifestBytes), ManifestBytes.length);
    async function Walk(Relative = '') {
        for (const Item of await fs.readdir(path.join(Runtime, Relative), {withFileTypes:true})) {
            const Name = Relative ? Relative + '/' + Item.name : Item.name;
            if (Item.isSymbolicLink()) throw Error('Linked runtime input: ' + Name);
            if (Item.isDirectory()) { if (/^Libs(\/(platforms|imageformats|iconengines|styles|generic|networkinformation|tls|sqldrivers|printsupport|resources|translations)(\/|$)|$)/.test(Name)) await Walk(Name); }
            else if (Allowed.test(Name)) await Add(Runtime, Name, Name);
        }
    }
    await Walk();
    if (Storage === 'qt-resource') {
        // Executable resources carry payloads; keep manifest and notices inspectable.
        // The separately compiled helper pin still controls runtime approval.
        for (const Name of [...Entries.keys()])
            if (Name.startsWith(BundlePrefix) && Name !== BundlePrefix + 'asset-manifest.json' && !Name.startsWith(BundlePrefix + 'notices/')) Entries.delete(Name);
    }
    for (const Name of ['HyperEditorWeb.exe','QtWebEngineProcess.exe','Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6WebEngineCore.dll','Qt6WebEngineWidgets.dll','Qt6WebChannel.dll','Libs/platforms/qwindows.dll','Libs/resources/icudtl.dat','Libs/resources/qtwebengine_resources.pak','Libs/resources/qtwebengine_resources_100p.pak','Libs/resources/qtwebengine_resources_200p.pak','Libs/resources/v8_context_snapshot.bin','Libs/translations/qtwebengine_locales/en-US.pak'])
        if (!Entries.has(Name)) throw Error('Required runtime input: ' + Name);
    if (Entries.size > 511) throw Error('Runtime inventory limit');
    const Inventory = {schemaVersion:1, sourceCommit:Lock.sourceCommit, profile:Manifest.profile, releaseEligible:false, bundleStorage:Storage,
        assetManifestSha256:Hash(ManifestBytes), files:[...Entries].sort(([A],[B]) => A < B ? -1 : A > B ? 1 : 0).map(([Name, Entry]) => ({path:Name, bytes:Entry.bytes, sha256:Entry.sha256}))};
    const InventoryBytes = Buffer.from(Json(Inventory));
    if (InventoryBytes.length > 256 * 1024) throw Error('Runtime inventory too large');
    await fs.mkdir(Output); // Never merge into or overwrite an existing package.
    for (const Entry of Inventory.files) {
        const Input = Entries.get(Entry.path);
        const Bytes = await Read(Input.root, Input.name);
        if (Hash(Bytes) !== Entry.sha256) throw Error('Input changed during staging: ' + Entry.path);
        const Target = path.join(Output, Entry.path);
        await fs.mkdir(path.dirname(Target), {recursive:true}); await fs.writeFile(Target, Bytes, {flag:'wx'});
    }
    let Inspection;
    if (Storage === 'qt-resource') {
        // Execute only the freshly copied local build, with no shell or renderer
        // arguments. A report proves resource consistency, never release trust.
        let Stdout;
        try {
            ({stdout:Stdout} = await ExecFile(path.join(Output, 'HyperEditorWeb.exe'),
                ['--verify-bundle-resources', Hash(ManifestBytes)],
                {cwd:Output, windowsHide:true, timeout:30000, maxBuffer:64 * 1024, encoding:'utf8'}));
        } catch (Failure) { throw new Error('Embedded helper inspection failed', {cause:Failure}); }
        try { Inspection = JSON.parse(Stdout); }
        catch (Failure) { throw new Error('Invalid embedded helper report', {cause:Failure}); }
        if (Inspection.schemaVersion !== 1 || Inspection.diagnosticOnly !== true
            || Inspection.bytesVerified !== true || Inspection.rendererCreated !== false
            || Inspection.manifestSha256 !== Hash(ManifestBytes) || Inspection.fileCount !== Manifest.files.length
            || typeof Inspection.productApproved !== 'boolean' || Inspection.releaseEligible !== Manifest.releaseEligible)
            throw Error('Embedded helper report mismatch');
        // Validate the staged bytes after execution too. No admission inventory
        // is published if dependencies or payloads changed during inspection.
        for (const Entry of Inventory.files) {
            const Bytes = await Read(Output, Entry.path);
            if (Bytes.length !== Entry.bytes || Hash(Bytes) !== Entry.sha256)
                throw Error('Staged input changed during inspection: ' + Entry.path);
        }
    }
    // Written last: interrupted candidates have no admission inventory.
    await fs.writeFile(path.join(Output, 'runtime-inventory.json'), InventoryBytes, {flag:'wx'});
    return {output:Output, sha256:Hash(InventoryBytes), files:Entries.size, releaseEligible:false, ...(Inspection ? {embeddedInspection:Inspection} : {})};
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    if (![5,6].includes(process.argv.length)) throw Error('Usage: stage-runtime.mjs <deployed-runtime> <bundle> <new-output> [files|qt-resource]');
    console.log(JSON.stringify(await BuildCandidate(...process.argv.slice(2))));
}
