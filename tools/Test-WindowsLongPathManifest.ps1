param([Parameter(Mandatory=$true)][string]$ListFile)
$ErrorActionPreference = 'Stop'
if (-not ('HcLongPathManifest' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class HcLongPathManifest {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    public static byte[] Read(string path) {
        IntPtr module = LoadLibraryExW(path, IntPtr.Zero, 0x22);
        if (module == IntPtr.Zero) throw new InvalidOperationException("Cannot inspect PE: " + path);
        try {
            IntPtr resource = FindResourceW(module, new IntPtr(1), new IntPtr(24));
            if (resource == IntPtr.Zero) throw new InvalidOperationException("Missing embedded manifest: " + path);
            uint size = SizeofResource(module, resource);
            if (size == 0 || size > 1048576) throw new InvalidOperationException("Invalid manifest size: " + path);
            IntPtr data = LockResource(LoadResource(module, resource));
            if (data == IntPtr.Zero) throw new InvalidOperationException("Cannot read manifest: " + path);
            byte[] result = new byte[size]; Marshal.Copy(data, result, 0, result.Length); return result;
        } finally { FreeLibrary(module); }
    }
}
"@
}
$count = 0
foreach ($entry in Get-Content -LiteralPath $ListFile -Encoding UTF8) {
    if (-not $entry.Trim()) { continue }
    $path = [IO.Path]::GetFullPath($entry)
    $bytes = [HcLongPathManifest]::Read($path)
    $stream = [IO.MemoryStream]::new($bytes, $false)
    $settings = [Xml.XmlReaderSettings]::new(); $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $reader = [Xml.XmlReader]::Create($stream, $settings)
    try {
        $xml = [Xml.XmlDocument]::new(); $xml.Load($reader)
        $ns = [Xml.XmlNamespaceManager]::new($xml.NameTable)
        $ns.AddNamespace('a','urn:schemas-microsoft-com:asm.v1')
        $ns.AddNamespace('v3','urn:schemas-microsoft-com:asm.v3')
        $ns.AddNamespace('w','http://schemas.microsoft.com/SMI/2016/WindowsSettings')
        $node = $xml.SelectSingleNode('/a:assembly/v3:application/v3:windowsSettings/w:longPathAware', $ns)
        if (-not $node -or $node.InnerText.Trim() -ine 'true') { throw "longPathAware is not enabled: $path" }
    } finally { $reader.Dispose(); $stream.Dispose() }
    ++$count
}
if (-not $count) { throw 'No executable manifests were checked.' }
Write-Output "Verified embedded longPathAware manifests: $count executables"
