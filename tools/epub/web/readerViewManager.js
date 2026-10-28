import DefaultViewManager from "epubjs/src/managers/default/index.js";

// Keep retired pages in the same DOM parent until their replacement is painted.
// Moving an iframe between parents would reload its browsing context.
export class ReaderViewManager extends DefaultViewManager {
    // Select per-spine layout only after the old view has been retired. Keep
    // the same Layout object so Rendition's location mapping remains coherent.
    createView(section, forceRight) {
        this.selectSection(section);
        const view=super.createView(section, forceRight);
        // EPUB.js shows a view before its final placement and location settle.
        // Keep it measurable while the retained old page covers the transition.
        if(this.transitionDepth)view.element.style.opacity="0";
        return view;
    }
    selectSection(section) {
        const policy=this.settings.sectionPolicy?.(section);
        if(!policy || !this.layout)return;
        const {layout,flow,spread,minSpreadWidth}=policy;
        const key=JSON.stringify(policy);
        if(this.sectionPolicyKey===key)return;
        this.sectionPolicyKey=key;
        this.layout.name=layout;this.layout.settings.layout=layout;
        this.layout.update({name:layout});this.layout.flow(flow);
        this.layout.settings.spread=spread;this.layout.spread(spread,minSpreadWidth);
        this.updateFlow(flow);
    }
    // The default scrolled-doc manager advances directly to another chapter.
    // Page commands first move one viewport with overlap inside the current frame.
    scrollMetrics() {
        const vertical=this.settings.axis==="vertical",element=this.container;
        const negative=!vertical && (this.writingMode==="vertical-rl" || getComputedStyle(element).direction==="rtl");
        const extent=Math.max(0,vertical?element.scrollHeight-element.clientHeight:element.scrollWidth-element.clientWidth);
        const offset=vertical?element.scrollTop:element.scrollLeft*(negative?-1:1);
        return {vertical,negative,extent,offset:Math.max(0,offset),size:vertical?element.clientHeight:element.clientWidth};
    }
    stepScrolled(direction) {
        if(this.isPaginated || !this.views?.length || this.layout.name==="pre-paginated")return false;
        const m=this.scrollMetrics(),target=Math.max(0,Math.min(m.extent,m.offset+direction*Math.max(1,Math.floor(m.size*0.9))));
        if(Math.abs(target-m.offset)<1)return false;
        this.scrollTo(m.vertical?0:target*(m.negative?-1:1),m.vertical?target:0,true);return true;
    }
    next() {if(this.stepScrolled(1))return Promise.resolve();return super.next();}
    prev() {
        if(this.stepScrolled(-1))return Promise.resolve();
        const index=this.views?.first()?.section.index;
        return Promise.resolve(super.prev()).then(result=>{
            if(!this.isPaginated && this.views?.first()?.section.index!==index && this.layout.name!=="pre-paginated") {
                const m=this.scrollMetrics();this.scrollTo(m.vertical?0:m.extent*(m.negative?-1:1),m.vertical?m.extent:0,true);
            }return result;
        });
    }
    // EPUB.js uses overflow:auto for both axes after a flow change. Restrict the
    // cross axis so centered frames/annotation SVGs cannot add a second scrollbar.
    updateOverflowAxis() {
        if(!this.container || this.isPaginated)return;
        this.container.style.overflowX=this.settings.axis==="vertical"?"hidden":"auto";
        this.container.style.overflowY=this.settings.axis==="vertical"?"auto":"hidden";
    }
    updateAxis(axis,force) {super.updateAxis(axis,force);this.updateOverflowAxis();}
    updateFlow(flow,...args) {
        super.updateFlow(flow,...args);
        // Flow updates reset the default manager axis, including when an
        // existing vertical-writing view has already reported its writing mode.
        if(this.writingMode) {
            const vertical=this.writingMode.startsWith("vertical");
            const axis=(this.isPaginated?vertical:!vertical)?"vertical":"horizontal";
            if(this.settings.axis!==axis){this.updateAxis(axis);this.updateLayout();}
        }
        this.updateOverflowAxis();
    }
    beginTransition() { this.transitionDepth = (this.transitionDepth || 0) + 1; }
    clear() {
        if (!this.transitionDepth || !this.views?.length) return super.clear();
        const bounds = this.container.getBoundingClientRect();
        this.retired ||= [];
        if(this.retired.length) {
            for(const view of this.views.all()){view.destroy();view.element.remove();}
            this.views._views=[];this.views.length=0;this.scrollTo(0,0,true);return;
        }
        for (const view of this.views.all()) {
            const element = view.element, rect = element.getBoundingClientRect();
            if (!view.displayed || rect.width <= 0 || rect.height <= 0) { view.destroy(); element.remove(); continue; }
            const inset = [bounds.top-rect.top, rect.right-bounds.right, rect.bottom-bounds.bottom, bounds.left-rect.left].map(n => `${Math.max(0,n)}px`);
            element.dataset.hcRetired = "true";
            Object.assign(element.style, { position:"fixed", left:`${rect.left}px`, top:`${rect.top}px`, width:`${rect.width}px`, height:`${rect.height}px`, zIndex:"10", pointerEvents:"none", clipPath:`inset(${inset.join(" ")})` });
            this.retired.push(view);
        }
        this.views._views = []; this.views.length = 0;
        this.scrollTo(0, 0, true);
    }
    paginatedLocation() {
        const locations=super.paginatedLocation();
        if(this.layout.name==="pre-paginated" || this.settings.axis!=="horizontal")return locations;
        const bounds=this.container.getBoundingClientRect();
        let used=0;
        for(const view of this.visible()) {
            const rect=view.position(),width=this.layout.width;
            let start,end,pageWidth;
            if(this.settings.direction==="rtl") {
                pageWidth=Math.min(Math.abs(bounds.right-rect.left),width)-used;
                end=rect.width-(rect.right-bounds.right)-used;start=end-pageWidth;
            } else {
                pageWidth=Math.min(rect.right-bounds.left,width)-used;
                start=bounds.left-rect.left+used;end=start+pageWidth;
            }
            used+=pageWidth;
            const location=locations.find(item=>item.index===view.section.index);if(!location)continue;
            // Fractional device pixels after centering/resizing must not turn an
            // exact column boundary (759.9999px) into the previous page number.
            let first=Math.floor((start+0.5)/this.layout.pageWidth),last=Math.floor((end+0.5)/this.layout.pageWidth);
            if(first<0){first=0;++last;}
            if(this.settings.direction==="rtl"){const old=first;first=location.totalPages-last;last=location.totalPages-old;}
            location.pages=[];for(let page=first+1;page<=last;++page)location.pages.push(page);
        }
        return locations;
    }
    endTransition() {
        this.transitionDepth = Math.max(0,(this.transitionDepth || 0)-1);
        if (!this.transitionDepth) {
            for(const view of this.views?.all() || [])view.element.style.removeProperty("opacity");
            this.releaseRetired();
        }
    }
    releaseRetired() {
        for (const view of this.retired || []) { view.destroy(); view.element.remove(); }
        this.retired = [];
    }
    destroy() { this.transitionDepth=0; this.releaseRetired(); super.destroy(); }
}
