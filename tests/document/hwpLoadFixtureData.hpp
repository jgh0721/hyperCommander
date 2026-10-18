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
    // Official @rhwp/core 0.8.7 npm assets, gitHead 1a76570e833917d15817415a53c09ad61ab3203f.
    // Test-only pins; package integrity receipt is tools/rhwp/engine-lock.json.
    inline const QByteArray EngineJsSha("70cde06a369fa7fd4fc8bc8f3d6acaee158596ba1a116a2c72159002b0b5654e");
    inline const QByteArray EngineWasmSha("54cf95014312e605c51b98eb9021247ea79ef2bbd0a7080e11c011d0246e3989");
    inline const QByteArray AdapterSha("9a53ffcaee570e35a9b571ac8255921cf09ea80debd0f700d069525e3dd29d30");
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
