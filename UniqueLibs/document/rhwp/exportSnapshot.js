// Capture one engine export transaction. The native receiver independently
// checks identity, report, hash and byte limits before admitting a storage write.
export async function ExportSnapshot(Doc, Format) {
    if (!['hwp','hwpx'].includes(Format)) throw new Error('export-format');
    let Output;
    try {
        Output = Format === 'hwp' ? Doc.exportHwpWithReport() : Doc.exportHwpxWithReport();
        const Text = Output.contentLoss();
        if (typeof Text !== 'string' || Text.length > 65536) throw new Error('export-report');
        const Loss = JSON.parse(Text);
        if (!Loss || Object.keys(Loss).sort().join(',') !== 'count,losses,outputFormat,schemaVersion'
            || Loss.schemaVersion !== 1 || Loss.outputFormat !== Format || Loss.count !== 0
            || !Array.isArray(Loss.losses) || Loss.losses.length) throw new Error('export-content-loss');
        // Both report and bytes belong to this exact DocumentExport object.
        const Bytes = Output.takeBytes();
        if (!(Bytes instanceof Uint8Array) || Bytes.length < 1 || Bytes.length > 64 * 1024 * 1024)
            throw new Error('export-budget');
        const Hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', Bytes)),
            Byte => Byte.toString(16).padStart(2,'0')).join('');
        return {format:Format,loss:Loss,sha256:Hash,bytes:Bytes};
    } finally { Output?.free(); }
}
