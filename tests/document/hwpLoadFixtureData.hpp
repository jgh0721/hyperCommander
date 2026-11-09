#pragma once
#include <QByteArray>
namespace hc::document::fixture
{
    inline const QByteArray Page("<!doctype html><canvas width='100' height='80'></canvas><script src='app.js'></script>");
    inline const QByteArray Script(R"JS(
new QWebChannel(qt.webChannelTransport, channel => {
    const bridge = channel.objects.loadBridge;
    bridge.PendingOpen(meta => {
        bridge.ReadChunk(meta.operation, 0, bytes => {
            if (!bytes) return;
            const identity = {...meta};
            for (const key of ['readOnly','byteLength','chunkSize','sha256','renderMode']) delete identity[key];
            bridge.Report(JSON.stringify({...identity,type:'Parsed',pageCount:1}), accepted => {
                if (!accepted) return;
                document.querySelector('canvas').getContext('2d').fillRect(0,0,100,80);
                requestAnimationFrame(() => requestAnimationFrame(() =>
                    bridge.Report(JSON.stringify({...identity,type:'FirstFrame',page:0,width:100,height:80}))));
            });
        });
    });
});
)JS");
    inline QByteArray DeferredScript(bool Hang)
    {
        auto Result=Script;
        Result.replace("pageCount:1", "pageCount:3");
        Result.replace("bridge.Report(JSON.stringify({...identity,type:'FirstFrame',page:0,width:100,height:80}))",
            "bridge.Report(JSON.stringify({...identity,type:'FirstFrame',page:0,width:100,height:80}), ok => { if(ok)setTimeout(()=>{bridge.Report(JSON.stringify({...identity,type:'ViewRenderBegin',page:1,sequence:'1'}), accepted=>{if(!accepted)return;" +
            QByteArray(Hang?"document.title='deferred-running';while(true){}":"bridge.Report(JSON.stringify({...identity,type:'ViewFailed'}));") + "});},600);})");
        return Result;
    }
    // Official @rhwp/core 0.9.0 npm assets, gitHead c9095c5f67632e6721e800ccc7fb43ea49d26682.
    // Test-only pins; package integrity receipt is tools/rhwp/engine-lock.json.
    inline const QByteArray EngineJsSha("02dcb4a17782f65814129c7971c81c05237e93e67327b205cfdb00b3a3ac0568");
    inline const QByteArray EngineWasmSha("c3b0632b84881f81f2c80be7c2f9e44a4086cb01e05a6a9107a3b3c6350abc1d");
    // Local adapter updated by 34539d6 (tabbed HWP viewer); upstream engine pins stay unchanged.
    inline const QByteArray AdapterSha("ebf883b5f6ac9d40c2ee43803cda8adc70cc0499cc538470be17b7f10043214a");
    inline const QByteArray EnginePage("<!doctype html><script type='module' src='app.js'></script>");
    inline const QByteArray EngineScript(R"JS(
import * as Engine from './rhwp.js';
import {OpenReadOnlyPages} from './adapter.js';
new QWebChannel(qt.webChannelTransport, channel => {
    OpenReadOnlyPages(channel.objects.loadBridge, Engine, () => {
        const canvas = document.createElement('canvas'); document.body.append(canvas); return canvas;
    })
        .then(() => { document.title = 'rendered'; })
        .catch(error => { document.title = 'failed:' + String(error); });
});
)JS");
}
