"""Build, sign, seal and compile the EPUB runtime policy from a locked checkout.

Uses a fresh output directory. Does not install, publish, or modify user settings.
The configured vendor SDKs and signing credentials remain external inputs.
"""
import argparse,hashlib,json,os,shutil,subprocess,sys
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kit",type=Path,required=True)
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--configure-arg",action="append",default=[],help="Explicit non-secret CMake -D input, repeatable")
    p.add_argument("--signing-cache",type=Path)
    p.add_argument("--build-product",action="store_true",help="Build the complete Release after compiling the new native pin")
    p.add_argument("--reuse-approved-image-plugin",action="store_true",help="EPUB-only build: keep the independently pinned shared qhcmodern.dll, rather than replace the image plugin")
    p.add_argument("--allow-dirty",action="store_true",help="Development only; record the dirty checkout in the receipt")
    a=p.parse_args();repo=Path(__file__).resolve().parents[2];kit=a.kit.resolve();output=a.output.resolve()
    private=(repo/"_build").resolve()
    if private not in output.parents:p.error("output must be inside this checkout's private _build directory")
    if a.signing_cache and private not in a.signing_cache.resolve().parents:p.error("signing-cache must be inside this checkout's private _build directory")
    for value in a.configure_arg:
        if value.startswith("-DHC_SIGNING_CACHE=") and private not in Path(value.split("=",1)[1]).resolve().parents:p.error("HC_SIGNING_CACHE must be inside this checkout's private _build directory")
    if os.name!="nt":p.error("Windows and the prescribed Qt/VS kit are required")
    status=subprocess.check_output(["git","status","--porcelain","--untracked-files=no"],cwd=repo).decode("utf-8")
    if status and not a.allow_dirty:p.error("Tracked changes exist; use a clean checkout, or explicit --allow-dirty for development")
    if any(not value.startswith("-D") or value.startswith(("-DHC_EPUB_APPROVED_INVENTORY_SHA256=","-DHC_EPUB_PRODUCT_RUNTIME_DIRECTORY=")) for value in a.configure_arg):
        p.error("configure-arg must be a -D input; EPUB runtime/pin are produced by this build")
    cmake=kit/"bin/qt-cmake.bat";node=shutil.which("node");npm=shutil.which("npm.cmd")
    if not cmake.is_file() or not node or not npm:p.error("Qt kit, Node and npm must be installed")
    output.mkdir(parents=True,exist_ok=False);web=repo/"tools/epub/web";build=repo/"_build/vc2026-qt612"
    startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    env=os.environ.copy();env["PATH"]=str(kit/"bin")+os.pathsep+env["PATH"]
    def run(name,args,cwd=repo):
        with (output/(name+".log")).open("wb") as log:
            result=subprocess.run(list(map(str,args)),cwd=cwd,env=env,stdout=log,stderr=subprocess.STDOUT,startupinfo=startup)
        if result.returncode:raise RuntimeError(f"{name} failed ({result.returncode}); see {output/(name+'.log')}")
        print(name+" passed",flush=True)
    query=build/".cmake/api/v1/query/codemodel-v2";query.parent.mkdir(parents=True,exist_ok=True);query.touch()
    common=[cmake,"--preset","vc2026-qt612","-DHC_BUILD_EPUB_PRODUCT=ON","-DHC_BUILD_EPUB_POC=ON","-DBUILD_TESTING=ON",f"-DHC_EPUB_PYTHON_EXECUTABLE={Path(sys.executable).resolve().as_posix()}",*a.configure_arg]
    run("npm-ci",[npm,"ci","--ignore-scripts","--no-audit","--no-fund","--cache",output/"npm-cache"],web)
    for name in ["testFontSources.mjs","testCustomCss.mjs","testReaderTools.mjs"]:run(name,[node,web/name])
    run("bundle",[node,web/"build.mjs",output/"bundle"])
    run("configure-helper",[*common,"-DHC_EPUB_PRODUCT_RUNTIME_DIRECTORY=","-DHC_EPUB_APPROVED_INVENTORY_SHA256="])
    run("helper",[cmake,"--build","--preset","vc2026-release","--target","HyperEpubWeb"])
    helper=build/"epub-product/Release/HyperEpubWeb.exe";helper_hash=hashlib.sha256(helper.read_bytes()).hexdigest()
    stage=[sys.executable,repo/"tools/epub/stage_owner_runtime.py","--product-candidate","--sign-product","--executable",helper,"--bundle",output/"bundle","--kit",kit,"--output",output/"runtime-signed"]
    if a.signing_cache:stage += ["--signing-cache",a.signing_cache.resolve()]
    run("stage",stage)
    manifest=output/"runtime-signed/epub-runtime-inventory.json";pin=hashlib.sha256(manifest.read_bytes()).hexdigest()
    run("signatures",["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File",repo/"tools/epub/Test-ProductRuntime.ps1","-RuntimeRoot",output/"runtime-signed","-ExpectedSha256",pin])
    run("configure-product",[*common,f"-DHC_EPUB_PRODUCT_RUNTIME_DIRECTORY={(output/'runtime-signed').as_posix()}",f"-DHC_EPUB_APPROVED_INVENTORY_SHA256={pin}"])
    run("policy-layout",[cmake,"--build","--preset","vc2026-release","--target","hc_epub_product_runtime"])
    reused=[]
    if a.build_product:
        reply=build/".cmake/api/v1/reply";index=json.loads(max(reply.glob("index-*.json"),key=lambda f:f.stat().st_mtime).read_text())
        model=json.loads((reply/index["reply"]["codemodel-v2"]["jsonFile"]).read_text());config=next(c for c in model["configurations"] if c["name"]=="Release")
        targets=[]
        for target in config["targets"]:
            detail=json.loads((reply/target["jsonFile"]).read_text())
            if detail["type"] in ["EXECUTABLE","SHARED_LIBRARY","MODULE_LIBRARY"]:targets.append(target["name"])
        # Deploy everything before sealing inventory; later Qt test deployment can refresh shared files.
        run("release",[cmake,"--build","--preset","vc2026-release","--target",*sorted(set(targets)),"hc_deploy"])
        cache={}
        for line in (build/"CMakeCache.txt").read_text(encoding="utf-8").splitlines():
            if line and not line.startswith(("#","//")) and "=" in line:
                key,value=line.split("=",1);cache[key.split(":",1)[0]]=value
        rhwp=cache.get("HC_RHWP_PRODUCT_RUNTIME_DIRECTORY","");rhwp_pin=cache.get("HC_RHWP_APPROVED_INVENTORY_SHA256","")
        if a.reuse_approved_image_plugin:
            if not rhwp or not rhwp_pin:raise RuntimeError("Shared image plugin reuse requires independent RHWP runtime and native pin")
            run("shared-runtime-signatures",["powershell","-NoProfile","-ExecutionPolicy","Bypass","-File",repo/"tools/rhwp/Test-ProductRuntime.ps1","-RuntimeRoot",rhwp,"-ExpectedSha256",rhwp_pin])
            relative="Libs/imageformats/qhcmodern.dll";source=Path(rhwp)/relative;destination=build/"Release"/relative
            manifest_bytes=(Path(rhwp)/"runtime-inventory.json").read_bytes()
            if hashlib.sha256(manifest_bytes).hexdigest()!=rhwp_pin:raise RuntimeError("Shared runtime changed after verification")
            entries=json.loads(manifest_bytes)["files"];expected=next(e["sha256"] for e in entries if e["path"]==relative)
            data=source.read_bytes()
            if hashlib.sha256(data).hexdigest()!=expected:raise RuntimeError("Approved image plugin changed")
            destination.parent.mkdir(parents=True,exist_ok=True);destination.write_bytes(data);reused.append({"path":relative,"sha256":expected})
        inventory=[cmake,f"-DSOURCE={(build/'Release').as_posix()}","-DCONFIG=Release",f"-DRHWP_SOURCE={rhwp}",f"-DRHWP_PIN={rhwp_pin}",f"-DEPUB_SOURCE={(output/'runtime-signed').as_posix()}",f"-DEPUB_PIN={pin}",f"-DEPUB_PYTHON={Path(sys.executable).resolve().as_posix()}",f"-DHC_VIEWER={(build/'Release/HyperViewer.dll').as_posix()}",f"-DPRODUCT_VERSION={cache['CMAKE_PROJECT_VERSION']}"]
        if cache.get("HC_PRODUCT_SIGNING")=="ON":inventory += [f"-DHC_SIGNING_SCRIPT={(repo/'tools/update/Sign-ProductFile.ps1').as_posix()}",f"-DHC_SIGNING_CACHE={cache.get('HC_SIGNING_CACHE',str(repo/'_build/signing-cache'))}"]
        run("inventory",[*inventory,"-P",repo/"cmake/GenerateProductInventory.cmake"])

    record={"schemaVersion":1,"sourceCommit":subprocess.check_output(["git","rev-parse","HEAD"],cwd=repo).decode().strip(),"trackedStatus":status,"kit":str(kit),"configureInputs":a.configure_arg,"helperInputSha256":helper_hash,"inventorySha256":pin,"runtime":str(output/"runtime-signed"),"productBuild":a.build_product,"reusedApprovedInputs":reused,"qualificationPolicy":"tools/epub/qualification-policy.json"}
    (output/"build-receipt.json").write_text(json.dumps(record,indent=2)+"\n",encoding="utf-8",newline="\r\n")
    print(json.dumps({"runtime":record["runtime"],"inventorySha256":pin,"productBuild":a.build_product}),flush=True)
    return 0
if __name__=="__main__":raise SystemExit(main())
