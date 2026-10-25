import DefaultViewManager from "epubjs/src/managers/default/index.js";

// Keep retired pages in the same DOM parent until their replacement is painted.
// Moving an iframe between parents would reload its browsing context.
export class ReaderViewManager extends DefaultViewManager {
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
        const locations=super.paginatedLocation(),bounds=this.container.getBoundingClientRect();
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
        if (!this.transitionDepth) this.releaseRetired();
    }
    releaseRetired() {
        for (const view of this.retired || []) { view.destroy(); view.element.remove(); }
        this.retired = [];
    }
    destroy() { this.transitionDepth=0; this.releaseRetired(); super.destroy(); }
}
