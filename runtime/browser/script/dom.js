/*
 * Arctic web engine: what a page's scripts see of the browser
 *
 * window, document, the elements and the rest of the web platform, written
 * over the natives of js.c (N): N knows lexbor's tree, the cookies, the
 * network and navigation; everything else is here. It is evaluated once per
 * page and returns a function that the engine calls with N and that gives
 * back the hooks the engine drives the page with.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
(function (N) {
'use strict';
const global = this;
/* the engine's own, kept: a page may put a polyfill of its own in their place */
const NativePromise = Promise;
const ELEMENT = 1, TEXT = 3, COMMENT = 8, DOCUMENT = 9, FRAGMENT = 11;

/* --- events --- */

const listeners = new WeakMap();

class Event {
    constructor(type, init) {
        init = init || {};
        this.type = String(type);
        this.bubbles = !!init.bubbles;
        this.cancelable = !!init.cancelable;
        this.composed = !!init.composed;
        this.defaultPrevented = false;
        this.target = null;
        this.currentTarget = null;
        this.eventPhase = 0;
        this.timeStamp = N.now();
        this.isTrusted = false;
        this._stop = false;
        this._stopNow = false;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = this._stopNow = true; }
    composedPath() { const path = []; for (let n = this.target; n; n = parentForEvent(n)) path.push(n); return path; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get srcElement() { return this.target; }
    initEvent(type, bubbles, cancelable) { this.type = type; this.bubbles = !!bubbles; this.cancelable = !!cancelable; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;

class CustomEvent extends Event {
    constructor(type, init) { super(type, init); this.detail = init && 'detail' in init ? init.detail : null; }
    initCustomEvent(type, bubbles, cancelable, detail) { this.initEvent(type, bubbles, cancelable); this.detail = detail; }
}
class UIEvent extends Event { constructor(t, i) { super(t, i); this.view = global; this.detail = (i && i.detail) || 0; } }
class MouseEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        for (const k of ['screenX', 'screenY', 'clientX', 'clientY', 'button', 'buttons'])
            this[k] = i[k] || 0;
        for (const k of ['ctrlKey', 'shiftKey', 'altKey', 'metaKey']) this[k] = !!i[k];
        this.relatedTarget = i.relatedTarget || null;
    }
    get pageX() { return this.clientX; } get pageY() { return this.clientY; }
}
class PointerEvent extends MouseEvent {}
class KeyboardEvent extends UIEvent {
    constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || ''; this.code = i.code || ''; this.keyCode = i.keyCode || 0; }
}
class FocusEvent extends UIEvent {}
class InputEvent extends UIEvent {}
class ErrorEvent extends Event { constructor(t, i) { super(t, i); Object.assign(this, i || {}); } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); this.data = i && i.data; this.origin = (i && i.origin) || ''; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i && i.state; } }
class PromiseRejectionEvent extends Event { constructor(t, i) { super(t, i); this.promise = i && i.promise; this.reason = i && i.reason; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); this.loaded = 0; this.total = 0; this.lengthComputable = false; } }

function parentForEvent(target) {
    if (target === global) return null;
    if (target === document) return global;
    if (target instanceof Node) return target.parentNode;
    return null;
}

class EventTarget {
    addEventListener(type, callback, options) {
        if (!callback) return;
        const capture = typeof options === 'boolean' ? options : !!(options && options.capture);
        const once = !!(options && typeof options === 'object' && options.once);
        let map = listeners.get(this);
        if (!map) listeners.set(this, map = {});
        const list = map[type] || (map[type] = []);
        if (!list.some(l => l.callback === callback && l.capture === capture)) list.push({ callback, capture, once });
    }
    removeEventListener(type, callback, options) {
        const capture = typeof options === 'boolean' ? options : !!(options && options.capture);
        const map = listeners.get(this);
        if (!map || !map[type]) return;
        map[type] = map[type].filter(l => !(l.callback === callback && l.capture === capture));
    }
    dispatchEvent(event) {
        event.target = this;
        const path = [];
        for (let n = parentForEvent(this); n; n = parentForEvent(n)) path.push(n);
        event.eventPhase = 1;
        for (let i = path.length - 1; i >= 0 && !event._stop; i--) invoke(path[i], event, true);
        event.eventPhase = 2;
        if (!event._stop) { invoke(this, event, true); invoke(this, event, false); }
        if (event.bubbles) {
            event.eventPhase = 3;
            for (let i = 0; i < path.length && !event._stop; i++) invoke(path[i], event, false);
        }
        event.eventPhase = 0;
        event.currentTarget = null;
        return !event.defaultPrevented;
    }
}

function invoke(target, event, capture) {
    event.currentTarget = target;
    const map = listeners.get(target);
    const list = map && map[event.type];
    if (list) {
        for (const l of list.slice()) {
            if (l.capture !== capture && event.eventPhase !== 2) continue;
            if (event.eventPhase === 2 && l.capture !== capture) continue;
            if (l.once) target.removeEventListener(event.type, l.callback, l.capture);
            try {
                if (typeof l.callback === 'function') l.callback.call(target, event);
                else if (l.callback && typeof l.callback.handleEvent === 'function') l.callback.handleEvent(event);
            } catch (e) { reportError(e); }
            if (event._stopNow) break;
        }
    }
    /* onload="" and the like, as properties */
    if (!capture || event.eventPhase === 2) {
        const handler = target['on' + event.type];
        if (typeof handler === 'function' && !(capture && event.eventPhase === 2)) {
            try { if (handler.call(target, event) === false) event.preventDefault(); } catch (e) { reportError(e); }
        }
    }
}

function reportError(e) {
    N.log('error: ' + (e && e.stack ? e + ' ' + e.stack : e));
    if (typeof global.onerror === 'function') {
        try { global.onerror(String(e), '', 0, 0, e); } catch (x) {}
    }
}

/* --- nodes --- */

function wrapList(array) { return Object.freeze(array); }

class NodeList {
    constructor(items) { this._items = items; for (let i = 0; i < items.length; i++) this[i] = items[i]; }
    get length() { return this._items.length; }
    item(i) { return this._items[i] || null; }
    forEach(f, self) { this._items.forEach(f, self); }
    entries() { return this._items.entries(); }
    keys() { return this._items.keys(); }
    values() { return this._items.values(); }
    [Symbol.iterator]() { return this._items[Symbol.iterator](); }
}
class HTMLCollection extends NodeList {
    namedItem(name) { return this._items.find(e => e.id === name || e.getAttribute('name') === name) || null; }
}

function children(node) {
    const list = [];
    for (let c = N.first(node); c; c = N.next(c)) list.push(c);
    return list;
}
function elementChildren(node) { return children(node).filter(c => N.type(c) === ELEMENT); }
function toNode(value) { return value instanceof Node ? value : N.createText(String(value)); }

class Node extends EventTarget {
    get nodeType() { return N.type(this); }
    get nodeName() {
        switch (N.type(this)) {
        case ELEMENT: return N.tag(this).toUpperCase();
        case TEXT: return '#text';
        case COMMENT: return '#comment';
        case DOCUMENT: return '#document';
        case FRAGMENT: return '#document-fragment';
        }
        return '';
    }
    get nodeValue() { const t = N.type(this); return t === TEXT || t === COMMENT ? N.text(this) : null; }
    set nodeValue(v) { const t = N.type(this); if (t === TEXT || t === COMMENT) N.setText(this, String(v)); }
    get ownerDocument() { return N.type(this) === DOCUMENT ? null : document; }
    get parentNode() { return N.parent(this); }
    get parentElement() { const p = N.parent(this); return p && N.type(p) === ELEMENT ? p : null; }
    get childNodes() { return new NodeList(children(this)); }
    get firstChild() { return N.first(this); }
    get lastChild() { return N.last(this); }
    get nextSibling() { return N.next(this); }
    get previousSibling() { return N.prev(this); }
    get isConnected() { let n = this; while (N.parent(n)) n = N.parent(n); return N.type(n) === DOCUMENT; }
    get textContent() { const t = N.type(this); return t === DOCUMENT ? null : N.text(this); }
    set textContent(v) { N.setText(this, v == null ? '' : String(v)); }
    hasChildNodes() { return !!N.first(this); }
    getRootNode() { let n = this; while (N.parent(n)) n = N.parent(n); return n; }
    contains(other) { for (let n = other; n; n = N.parent(n)) if (n === this) return true; return false; }
    appendChild(child) {
        if (N.type(child) === FRAGMENT) { for (const c of children(child)) this.appendChild(c); return child; }
        N.append(this, child); inserted(child); return child;
    }
    insertBefore(child, ref) {
        if (N.type(child) === FRAGMENT) { for (const c of children(child)) this.insertBefore(c, ref); return child; }
        N.insertBefore(this, child, ref || null); inserted(child); return child;
    }
    removeChild(child) { N.remove(child); return child; }
    replaceChild(child, old) { this.insertBefore(child, old); N.remove(old); return old; }
    cloneNode(deep) { return N.clone(this, !!deep); }
    isEqualNode(other) { return other instanceof Node && N.outer(this) === N.outer(other); }
    isSameNode(other) { return this === other; }
    compareDocumentPosition(other) {
        if (this === other) return 0;
        if (this.contains(other)) return 20;
        if (other.contains(this)) return 10;
        const order = all(document);
        return order.indexOf(this) < order.indexOf(other) ? 4 : 2;
    }
    normalize() {}
}
Object.assign(Node, { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, COMMENT_NODE: 8,
    DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11, DOCUMENT_POSITION_DISCONNECTED: 1,
    DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4, DOCUMENT_POSITION_CONTAINS: 8,
    DOCUMENT_POSITION_CONTAINED_BY: 16 });

/* elements in tree order under a node */
function all(root) {
    const list = [];
    const walk = n => { for (let c = N.first(n); c; c = N.next(c)) if (N.type(c) === ELEMENT) { list.push(c); walk(c); } };
    walk(root);
    return list;
}

/* a script put into the page runs, as in a browser; one already run does not */
const pendingScripts = [];
let inlineModules = 0;
function inserted(node) {
    if (N.type(node) !== ELEMENT || !node.isConnected) return;
    const scripts = N.tag(node) === 'script' ? [node] : N.select(node, 'script', false);
    for (const s of scripts) {
        if (N.attr(s, 'data-arctic-ran') !== null) continue;
        N.setAttr(s, 'data-arctic-ran', '');
        pendingScripts.push(s);
        setTimeout(runPendingScripts, 0);
    }
}
function runPendingScripts() {
    while (pendingScripts.length) {
        const s = pendingScripts.shift();
        const type = (N.attr(s, 'type') || '').toLowerCase();
        if (type && !/^(text|application)\/(x-)?(java|ecma)script$|^module$/.test(type)) continue;
        const src = N.attr(s, 'src');
        const previous = N.currentScript();
        N.setCurrentScript(s);
        if (src) {
            const r = N.fetch('GET', src);
            if (r.status >= 200 && r.status < 400) {
                if (type === 'module') N.evalModule(r.text, r.url); else N.eval(r.text, r.url);
                s.dispatchEvent(new Event('load'));
            } else s.dispatchEvent(new Event('error'));
        } else if (type === 'module') N.evalModule(N.text(s), N.url() + '#module-' + (++inlineModules));
        else N.eval(N.text(s), 'inline');
        N.setCurrentScript(previous);
    }
}

class CharacterData extends Node {
    get data() { return N.text(this); }
    set data(v) { N.setText(this, String(v)); }
    get length() { return N.text(this).length; }
    appendData(s) { this.data += s; }
    remove() { N.remove(this); }
}
class Text extends CharacterData {
    constructor(text) { super(); return N.createText(text === undefined ? '' : String(text)); }
    get wholeText() { return this.data; }
}
class Comment extends CharacterData {
    constructor(text) { super(); return N.createComment(text === undefined ? '' : String(text)); }
}

/* the parts of a node that hold elements: element, document, fragment */
const ParentNode = {
    get children() { return new HTMLCollection(elementChildren(this)); },
    get childElementCount() { return elementChildren(this).length; },
    get firstElementChild() { return elementChildren(this)[0] || null; },
    get lastElementChild() { const c = elementChildren(this); return c[c.length - 1] || null; },
    querySelector(selector) { return N.select(this, String(selector), true)[0] || null; },
    querySelectorAll(selector) { return new NodeList(N.select(this, String(selector), false)); },
    getElementsByTagName(tag) {
        tag = String(tag).toLowerCase();
        return new HTMLCollection(tag === '*' ? all(this) : all(this).filter(e => N.tag(e) === tag));
    },
    getElementsByTagNameNS(ns, tag) { return this.getElementsByTagName(tag); },
    getElementsByClassName(names) {
        const wanted = String(names).split(/\s+/).filter(Boolean);
        return new HTMLCollection(all(this).filter(e => {
            const have = (N.attr(e, 'class') || '').split(/\s+/);
            return wanted.every(w => have.includes(w));
        }));
    },
    append(...nodes) { for (const n of nodes) this.appendChild(toNode(n)); },
    prepend(...nodes) { const first = N.first(this); for (const n of nodes) this.insertBefore(toNode(n), first); },
    replaceChildren(...nodes) { while (N.first(this)) N.remove(N.first(this)); this.append(...nodes); },
};
function mixin(target, source) {
    for (const key of Object.getOwnPropertyNames(source))
        Object.defineProperty(target.prototype, key, Object.getOwnPropertyDescriptor(source, key));
}

const ChildNode = {
    before(...nodes) { const p = N.parent(this); if (p) for (const n of nodes) p.insertBefore(toNode(n), this); },
    after(...nodes) { const p = N.parent(this), next = N.next(this); if (p) for (const n of nodes) p.insertBefore(toNode(n), next); },
    replaceWith(...nodes) { this.before(...nodes); N.remove(this); },
    remove() { N.remove(this); },
    get nextElementSibling() { for (let n = N.next(this); n; n = N.next(n)) if (N.type(n) === ELEMENT) return n; return null; },
    get previousElementSibling() { for (let n = N.prev(this); n; n = N.prev(n)) if (N.type(n) === ELEMENT) return n; return null; },
};
mixin(CharacterData, ChildNode);

/* --- style="" as an object --- */

function cssName(prop) {
    if (prop === 'cssFloat') return 'float';
    return prop.replace(/[A-Z]/g, m => '-' + m.toLowerCase()).replace(/^(webkit|moz|ms)-/, '-$1-');
}
function parseStyle(text) {
    const map = new Map();
    for (const part of (text || '').split(';')) {
        const i = part.indexOf(':');
        if (i > 0) map.set(part.slice(0, i).trim().toLowerCase(), part.slice(i + 1).trim());
    }
    return map;
}
class CSSStyleDeclaration {
    constructor(element) { this._element = element; }
    _map() { return parseStyle(this._element ? N.attr(this._element, 'style') : ''); }
    _store(map) {
        if (!this._element) return;
        const text = [...map].map(([k, v]) => k + ': ' + v).join('; ');
        N.setAttr(this._element, 'style', text);
    }
    getPropertyValue(name) { return this._map().get(String(name).toLowerCase()) || ''; }
    getPropertyPriority() { return ''; }
    setProperty(name, value) {
        const map = this._map();
        if (value === null || value === undefined || value === '') map.delete(String(name).toLowerCase());
        else map.set(String(name).toLowerCase(), String(value));
        this._store(map);
    }
    removeProperty(name) { const v = this.getPropertyValue(name); const m = this._map(); m.delete(String(name).toLowerCase()); this._store(m); return v; }
    get cssText() { return this._element ? N.attr(this._element, 'style') || '' : ''; }
    set cssText(v) { if (this._element) N.setAttr(this._element, 'style', String(v)); }
    get length() { return this._map().size; }
    item(i) { return [...this._map().keys()][i] || ''; }
}
function styleProxy(element) {
    return new Proxy(new CSSStyleDeclaration(element), {
        get(target, prop) {
            if (typeof prop !== 'string' || prop in target) return Reflect.get(target, prop, target);
            return target.getPropertyValue(cssName(prop));
        },
        set(target, prop, value) {
            if (typeof prop !== 'string' || prop in target) return Reflect.set(target, prop, value, target);
            target.setProperty(cssName(prop), value);
            return true;
        },
        has(target, prop) { return true; },
    });
}

/* --- elements --- */

class DOMTokenList {
    constructor(element, attribute) { this._element = element; this._attribute = attribute; }
    _list() { return (N.attr(this._element, this._attribute) || '').split(/\s+/).filter(Boolean); }
    _set(list) { N.setAttr(this._element, this._attribute, list.join(' ')); }
    get length() { return this._list().length; }
    get value() { return N.attr(this._element, this._attribute) || ''; }
    item(i) { return this._list()[i] || null; }
    contains(token) { return this._list().includes(String(token)); }
    add(...tokens) { const l = this._list(); for (const t of tokens) if (!l.includes(t)) l.push(String(t)); this._set(l); }
    remove(...tokens) { this._set(this._list().filter(t => !tokens.includes(t))); }
    toggle(token, force) {
        const has = this.contains(token);
        const want = force === undefined ? !has : !!force;
        if (want && !has) this.add(token); else if (!want && has) this.remove(token);
        return want;
    }
    replace(a, b) { const l = this._list(); const i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; }
    supports() { return true; }
    forEach(f, self) { this._list().forEach(f, self); }
    [Symbol.iterator]() { return this._list()[Symbol.iterator](); }
    toString() { return this.value; }
}

const datasetProxy = element => new Proxy({}, {
    get(t, prop) { return typeof prop === 'string' ? N.attr(element, 'data-' + cssName(prop)) ?? undefined : undefined; },
    set(t, prop, value) { N.setAttr(element, 'data-' + cssName(prop), String(value)); return true; },
    deleteProperty(t, prop) { N.removeAttr(element, 'data-' + cssName(prop)); return true; },
    has(t, prop) { return N.attr(element, 'data-' + cssName(prop)) !== null; },
    ownKeys() { return N.attrs(element).filter(a => a[0].startsWith('data-')).map(a => a[0].slice(5).replace(/-([a-z])/g, (m, c) => c.toUpperCase())); },
    getOwnPropertyDescriptor(t, prop) { const v = N.attr(element, 'data-' + cssName(prop)); return v === null ? undefined : { value: v, enumerable: true, configurable: true }; },
});

const extras = new WeakMap();
function extra(element) { let e = extras.get(element); if (!e) extras.set(element, e = {}); return e; }

class Element extends Node {
    get tagName() { return N.tag(this).toUpperCase(); }
    get localName() { return N.tag(this); }
    get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }
    get prefix() { return null; }
    get id() { return N.attr(this, 'id') || ''; }
    set id(v) { N.setAttr(this, 'id', String(v)); }
    get className() { return N.attr(this, 'class') || ''; }
    set className(v) { N.setAttr(this, 'class', String(v)); }
    get classList() { const x = extra(this); return x.classList || (x.classList = new DOMTokenList(this, 'class')); }
    get attributes() {
        const list = N.attrs(this).map(([name, value]) => ({ name, value, localName: name, nodeName: name, nodeValue: value, specified: true }));
        list.getNamedItem = name => list.find(a => a.name === name) || null;
        list.item = i => list[i] || null;
        return list;
    }
    getAttribute(name) { return N.attr(this, String(name).toLowerCase()); }
    getAttributeNS(ns, name) { return this.getAttribute(name); }
    setAttribute(name, value) { N.setAttr(this, String(name).toLowerCase(), String(value)); }
    setAttributeNS(ns, name, value) { this.setAttribute(name, value); }
    removeAttribute(name) { N.removeAttr(this, String(name).toLowerCase()); }
    removeAttributeNS(ns, name) { this.removeAttribute(name); }
    hasAttribute(name) { return N.attr(this, String(name).toLowerCase()) !== null; }
    hasAttributes() { return N.attrs(this).length > 0; }
    getAttributeNames() { return N.attrs(this).map(a => a[0]); }
    toggleAttribute(name, force) {
        const has = this.hasAttribute(name), want = force === undefined ? !has : !!force;
        if (want && !has) this.setAttribute(name, ''); else if (!want && has) this.removeAttribute(name);
        return want;
    }
    get innerHTML() { return N.inner(this); }
    set innerHTML(html) { N.setInner(this, html == null ? '' : String(html)); }
    get outerHTML() { return N.outer(this); }
    set outerHTML(html) {
        const p = N.parent(this); if (!p) return;
        const box = N.create('div'); N.setInner(box, String(html));
        for (const c of children(box)) p.insertBefore(c, this);
        N.remove(this);
    }
    insertAdjacentHTML(where, html) {
        const box = N.create('div'); N.setInner(box, String(html));
        for (const c of children(box)) this.insertAdjacentElement(where, c);
    }
    insertAdjacentElement(where, node) {
        switch (String(where).toLowerCase()) {
        case 'beforebegin': if (N.parent(this)) N.parent(this).insertBefore(node, this); break;
        case 'afterbegin': this.insertBefore(node, N.first(this)); break;
        case 'beforeend': this.appendChild(node); break;
        case 'afterend': if (N.parent(this)) N.parent(this).insertBefore(node, N.next(this)); break;
        }
        return node;
    }
    insertAdjacentText(where, text) { this.insertAdjacentElement(where, N.createText(String(text))); }
    matches(selector) { return N.matches(this, String(selector)); }
    webkitMatchesSelector(selector) { return this.matches(selector); }
    closest(selector) { for (let n = this; n && N.type(n) === ELEMENT; n = N.parent(n)) if (N.matches(n, selector)) return n; return null; }
    getBoundingClientRect() { return new DOMRect(0, 0, 0, 0); }
    getClientRects() { return []; }
    scrollIntoView() {} scroll() {} scrollTo() {} scrollBy() {}
    get scrollTop() { return 0; } set scrollTop(v) {}
    get scrollLeft() { return 0; } set scrollLeft(v) {}
    get scrollWidth() { return 0; } get scrollHeight() { return 0; }
    get clientWidth() { return 0; } get clientHeight() { return 0; }
    get clientTop() { return 0; } get clientLeft() { return 0; }
    attachShadow() { const f = N.createFragment(); extra(this).shadowRoot = f; return f; }
    get shadowRoot() { return extra(this).shadowRoot || null; }
    animate() { return { finished: Promise.resolve(), cancel() {}, play() {}, pause() {}, addEventListener() {} }; }
    getAnimations() { return []; }
    setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false; }
    requestFullscreen() { return Promise.reject(new Error('no fullscreen')); }
}
mixin(Element, ParentNode);
mixin(Element, ChildNode);

function reflect(cls, props) {
    for (const p of props) {
        const attribute = p.toLowerCase();
        Object.defineProperty(cls.prototype, p, {
            get() { return N.attr(this, attribute) || ''; },
            set(v) { N.setAttr(this, attribute, String(v)); },
            configurable: true,
        });
    }
}
function reflectBool(cls, props) {
    for (const p of props) {
        const attribute = p.toLowerCase();
        Object.defineProperty(cls.prototype, p, {
            get() { return N.attr(this, attribute) !== null; },
            set(v) { if (v) N.setAttr(this, attribute, ''); else N.removeAttr(this, attribute); },
            configurable: true,
        });
    }
}
function reflectURL(cls, props) {
    for (const p of props) {
        Object.defineProperty(cls.prototype, p, {
            get() { const v = N.attr(this, p.toLowerCase()); return v === null ? '' : (N.resolve(v) || v); },
            set(v) { N.setAttr(this, p.toLowerCase(), String(v)); },
            configurable: true,
        });
    }
}

class HTMLElement extends Element {
    get style() { const x = extra(this); return x.style || (x.style = styleProxy(this)); }
    set style(v) { N.setAttr(this, 'style', String(v)); }
    get dataset() { const x = extra(this); return x.dataset || (x.dataset = datasetProxy(this)); }
    get innerText() { return N.text(this); }
    set innerText(v) { N.setText(this, v == null ? '' : String(v)); }
    get outerText() { return N.text(this); }
    get offsetParent() { return this.parentElement; }
    get offsetTop() { return 0; } get offsetLeft() { return 0; }
    /* until the layout answers scripts, a block is as wide as the window and a line tall */
    get offsetWidth() { return this.isConnected && N.attr(this, 'hidden') === null ? N.width : 0; }
    get offsetHeight() { return this.isConnected && N.attr(this, 'hidden') === null ? 18 : 0; }
    get clientWidth() { return this.offsetWidth; } get clientHeight() { return this.offsetHeight; }
    getBoundingClientRect() { return new DOMRect(0, 0, this.offsetWidth, this.offsetHeight); }
    get tabIndex() { const v = N.attr(this, 'tabindex'); return v === null ? -1 : parseInt(v, 10) || 0; }
    set tabIndex(v) { N.setAttr(this, 'tabindex', String(v)); }
    click() { this.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true })); }
    focus() { document._active = this; this.dispatchEvent(new FocusEvent('focus')); }
    blur() { if (document._active === this) document._active = null; this.dispatchEvent(new FocusEvent('blur')); }
    get isContentEditable() { return false; }
    get contentEditable() { return 'inherit'; }
}
reflect(HTMLElement, ['title', 'lang', 'dir', 'accessKey', 'slot', 'nonce', 'role']);
reflectBool(HTMLElement, ['hidden', 'inert', 'draggable', 'spellcheck', 'translate', 'autofocus']);

const classes = {};
function defineElement(name, tags, body) {
    const cls = body || class extends HTMLElement {};
    Object.defineProperty(cls, 'name', { value: name });
    classes[name] = cls;
    for (const t of tags) tagClasses[t] = cls;
    return cls;
}
const tagClasses = {};

defineElement('HTMLDivElement', ['div']);
defineElement('HTMLSpanElement', ['span']);
defineElement('HTMLParagraphElement', ['p']);
defineElement('HTMLHeadingElement', ['h1', 'h2', 'h3', 'h4', 'h5', 'h6']);
defineElement('HTMLUListElement', ['ul']);
defineElement('HTMLOListElement', ['ol']);
defineElement('HTMLLIElement', ['li']);
defineElement('HTMLBRElement', ['br']);
defineElement('HTMLHRElement', ['hr']);
defineElement('HTMLPreElement', ['pre']);
defineElement('HTMLTableElement', ['table']);
defineElement('HTMLTableRowElement', ['tr']);
defineElement('HTMLTableCellElement', ['td', 'th']);
defineElement('HTMLTableSectionElement', ['thead', 'tbody', 'tfoot']);
defineElement('HTMLLabelElement', ['label']);
defineElement('HTMLHtmlElement', ['html']);
defineElement('HTMLHeadElement', ['head']);
defineElement('HTMLBodyElement', ['body']);
defineElement('HTMLTitleElement', ['title']);
defineElement('HTMLMetaElement', ['meta']);
defineElement('HTMLStyleElement', ['style']);
defineElement('HTMLNavElement', []);
defineElement('HTMLUnknownElement', []);
defineElement('HTMLPictureElement', ['picture']);
defineElement('HTMLSourceElement', ['source']);
defineElement('HTMLDialogElement', ['dialog'], class extends HTMLElement {
    show() { this.setAttribute('open', ''); } showModal() { this.setAttribute('open', ''); } close() { this.removeAttribute('open'); }
    get open() { return this.hasAttribute('open'); }
});
defineElement('HTMLTemplateElement', ['template'], class extends HTMLElement {
    get content() { const f = N.createFragment(); for (const c of children(this)) N.append(f, N.clone(c, true)); return f; }
});
const HTMLAnchorElement = defineElement('HTMLAnchorElement', ['a', 'area'], class extends HTMLElement {
    toString() { return this.href; }
});
reflectURL(HTMLAnchorElement, ['href']);
reflect(HTMLAnchorElement, ['target', 'rel', 'download', 'hreflang', 'type', 'ping']);
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin'])
    Object.defineProperty(HTMLAnchorElement.prototype, part, { get() { try { return new URL(this.href)[part]; } catch (e) { return ''; } } });
const HTMLLinkElement = defineElement('HTMLLinkElement', ['link']);
reflectURL(HTMLLinkElement, ['href']);
reflect(HTMLLinkElement, ['rel', 'as', 'type', 'media', 'crossOrigin', 'integrity']);
const HTMLScriptElement = defineElement('HTMLScriptElement', ['script'], class extends HTMLElement {
    get text() { return N.text(this); } set text(v) { N.setText(this, String(v)); }
});
reflectURL(HTMLScriptElement, ['src']);
reflect(HTMLScriptElement, ['type', 'charset', 'crossOrigin', 'integrity', 'referrerPolicy']);
reflectBool(HTMLScriptElement, ['async', 'defer', 'noModule']);
const HTMLImageElement = defineElement('HTMLImageElement', ['img'], class extends HTMLElement {
    get complete() { return true; }
    get naturalWidth() { return 0; } get naturalHeight() { return 0; }
    get width() { return parseInt(N.attr(this, 'width'), 10) || 0; } set width(v) { N.setAttr(this, 'width', String(v)); }
    get height() { return parseInt(N.attr(this, 'height'), 10) || 0; } set height(v) { N.setAttr(this, 'height', String(v)); }
    get currentSrc() { return this.src; }
    decode() { return Promise.resolve(); }
    set src(v) {
        N.setAttr(this, 'src', String(v));
        /* a tracking pixel of a script loads nothing here; it reports success to whoever waits */
        setTimeout(() => this.dispatchEvent(new Event('load')), 0);
    }
    get src() { const v = N.attr(this, 'src'); return v === null ? '' : (N.resolve(v) || v); }
});
reflect(HTMLImageElement, ['alt', 'srcset', 'sizes', 'loading', 'decoding', 'crossOrigin', 'referrerPolicy', 'useMap']);
/* the window of another document: a page reaches it by postMessage, as across origins */
/* ports given away with a message: the end that stays here is reached by an id from now on */
function transferPorts(transfer) {
    const ids = [];
    for (const port of transfer || []) {
        if (!(port instanceof MessagePort) || !port._other) continue;
        const kept = port._other;
        kept._id = N.portRegister();
        portsById.set(kept._id, kept);
        port._transferred = true;
        ids.push(kept._id);
    }
    return ids;
}
function remoteWindow(post) {
    const w = {
        postMessage(data, targetOrigin, transfer) {
            let json;
            try { json = JSON.stringify(data === undefined ? null : data); } catch (e) { throw new DOMException('could not clone', 'DataCloneError'); }
            const options = typeof targetOrigin === 'object' && targetOrigin ? targetOrigin : null;
            if (options) transfer = options.transfer;
            post(json, options ? String(options.targetOrigin || '/') : String(targetOrigin || '*'), transferPorts(transfer));
        },
        closed: false,
        close() {}, focus() {}, blur() {},
    };
    for (const name of ['location', 'document']) Object.defineProperty(w, name, { get() { throw new DOMException('Blocked a frame from accessing a cross-origin frame.', 'SecurityError'); } });
    w.window = w.self = w.frames = w;
    return w;
}
/* a frame of about:blank is of this page's origin: its window, to the page, has what a window
 * has - libraries take a pristine JSON or Array from one - in this window's realm */
function blankWindow(iframe) {
    const w = Object.create(global);
    Object.assign(w, {
        parent: window, top: window.top, frameElement: iframe, opener: null, closed: false,
        postMessage(data, targetOrigin, transfer) {
            const json = JSON.stringify(data === undefined ? null : data);
            N.postFrame(iframe, json, typeof targetOrigin === 'string' ? targetOrigin : '*', transferPorts(transfer));
        },
    });
    w.window = w.self = w.frames = w.globalThis = w;
    return w;
}
function isBlankFrame(iframe) {
    const src = N.attr(iframe, 'src');
    return !src || src === 'about:blank' || /^javascript:/i.test(src);
}
defineElement('HTMLIFrameElement', ['iframe'], class extends HTMLElement {
    /* one window for the frame whatever it shows, as in a browser: blank of this origin, or another's */
    get contentWindow() {
        if (!this.isConnected) return null;
        const x = extra(this), iframe = this;
        if (!x.contentWindow) {
            const pick = () => isBlankFrame(iframe)
                ? (x.blankWindow || (x.blankWindow = blankWindow(iframe)))
                : (x.remoteWindow || (x.remoteWindow = remoteWindow((json, origin, ports) => N.postFrame(iframe, json, origin, ports))));
            x.contentWindow = new Proxy({}, {
                get(t, p) { const w = pick(); const v = w[p]; return typeof v === 'function' && p !== 'constructor' ? v.bind(w) : v; },
                set(t, p, v) { pick()[p] = v; return true; },
                has(t, p) { return p in pick(); },
            });
        }
        return x.contentWindow;
    }
    get contentDocument() { return null; }
});
reflectURL(classes.HTMLIFrameElement, ['src']);
reflect(classes.HTMLIFrameElement, ['name', 'width', 'height', 'allow', 'frameBorder', 'scrolling', 'referrerPolicy', 'loading', 'srcdoc', 'sandbox']);
reflectBool(classes.HTMLIFrameElement, ['allowFullscreen']);
defineElement('HTMLCanvasElement', ['canvas'], class extends HTMLElement {
    get width() { return parseInt(N.attr(this, 'width'), 10) || 300; } set width(v) { N.setAttr(this, 'width', String(v)); }
    get height() { return parseInt(N.attr(this, 'height'), 10) || 150; } set height(v) { N.setAttr(this, 'height', String(v)); }
    getContext(kind) { return kind === '2d' ? canvasContext(this) : null; }
    toDataURL() { return 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=='; }
    toBlob(callback) { setTimeout(() => callback(new Blob([])), 0); }
});
function canvasContext(canvas) {
    const noop = () => {};
    const ctx = { canvas, fillStyle: '#000', strokeStyle: '#000', font: '10px sans-serif', textBaseline: 'alphabetic',
        textAlign: 'start', globalAlpha: 1, lineWidth: 1, globalCompositeOperation: 'source-over' };
    for (const m of ['fillRect', 'clearRect', 'strokeRect', 'beginPath', 'closePath', 'moveTo', 'lineTo', 'arc', 'arcTo',
        'rect', 'fill', 'stroke', 'save', 'restore', 'translate', 'rotate', 'scale', 'transform', 'setTransform',
        'fillText', 'strokeText', 'drawImage', 'putImageData', 'clip', 'quadraticCurveTo', 'bezierCurveTo', 'setLineDash',
        'ellipse', 'resetTransform'])
        ctx[m] = noop;
    ctx.measureText = text => ({ width: String(text).length * 6, actualBoundingBoxAscent: 8, actualBoundingBoxDescent: 2 });
    ctx.getImageData = (x, y, w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(Math.max(0, w * h * 4)) });
    ctx.createImageData = (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(Math.max(0, w * h * 4)) });
    ctx.isPointInPath = () => false;
    ctx.createLinearGradient = ctx.createRadialGradient = ctx.createPattern = () => ({ addColorStop: noop });
    ctx.getLineDash = () => [];
    return ctx;
}

/* form controls: their value is the attribute, as the engine draws and sends it */
const HTMLInputElement = defineElement('HTMLInputElement', ['input'], class extends HTMLElement {
    get value() { return N.attr(this, 'value') || ''; } set value(v) { N.setAttr(this, 'value', v == null ? '' : String(v)); }
    get defaultValue() { return N.attr(this, 'value') || ''; } set defaultValue(v) { N.setAttr(this, 'value', String(v)); }
    get type() { return (N.attr(this, 'type') || 'text').toLowerCase(); } set type(v) { N.setAttr(this, 'type', String(v)); }
    get form() { return this.closest('form'); }
    get labels() { return new NodeList([]); }
    select() {} setSelectionRange() {} setCustomValidity() {}
    checkValidity() { return true; } reportValidity() { return true; }
    get validity() { return { valid: true }; }
    get selectionStart() { return this.value.length; } get selectionEnd() { return this.value.length; }
    get files() { return []; }
});
reflect(HTMLInputElement, ['name', 'placeholder', 'autocomplete', 'min', 'max', 'step', 'pattern', 'accept', 'inputMode']);
reflectBool(HTMLInputElement, ['checked', 'disabled', 'readOnly', 'required', 'multiple']);
Object.defineProperty(HTMLInputElement.prototype, 'defaultChecked', Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'checked'));
Object.defineProperty(HTMLInputElement.prototype, 'maxLength', { get() { return parseInt(N.attr(this, 'maxlength'), 10) || -1; }, set(v) { N.setAttr(this, 'maxlength', String(v)); } });
const HTMLTextAreaElement = defineElement('HTMLTextAreaElement', ['textarea'], class extends HTMLElement {
    get value() { const v = N.attr(this, 'value'); return v === null ? N.text(this) : v; }
    set value(v) { N.setAttr(this, 'value', v == null ? '' : String(v)); }
    get form() { return this.closest('form'); }
    select() {} setSelectionRange() {}
});
reflect(HTMLTextAreaElement, ['name', 'placeholder']);
reflectBool(HTMLTextAreaElement, ['disabled', 'readOnly', 'required']);
const HTMLButtonElement = defineElement('HTMLButtonElement', ['button'], class extends HTMLElement {
    get type() { return (N.attr(this, 'type') || 'submit').toLowerCase(); }
    get form() { return this.closest('form'); }
    get value() { return N.attr(this, 'value') || ''; } set value(v) { N.setAttr(this, 'value', String(v)); }
});
reflect(HTMLButtonElement, ['name']);
reflectBool(HTMLButtonElement, ['disabled']);
const HTMLOptionElement = defineElement('HTMLOptionElement', ['option'], class extends HTMLElement {
    get value() { const v = N.attr(this, 'value'); return v === null ? N.text(this).trim() : v; }
    set value(v) { N.setAttr(this, 'value', String(v)); }
    get text() { return N.text(this).trim(); }
    get index() { const s = this.closest('select'); return s ? [...s.options].indexOf(this) : 0; }
});
reflectBool(HTMLOptionElement, ['selected', 'disabled', 'defaultSelected']);
defineElement('HTMLSelectElement', ['select'], class extends HTMLElement {
    get options() { return new HTMLCollection(N.select(this, 'option', false)); }
    get selectedIndex() { const o = [...this.options]; const i = o.findIndex(x => x.selected); return i >= 0 ? i : (o.length ? 0 : -1); }
    set selectedIndex(i) { [...this.options].forEach((o, k) => o.selected = k === i); }
    get value() { const o = this.options[this.selectedIndex]; return o ? o.value : ''; }
    set value(v) { [...this.options].forEach(o => o.selected = o.value === String(v)); }
    get length() { return this.options.length; }
    get form() { return this.closest('form'); }
    add(option) { this.appendChild(option); }
});
reflect(classes.HTMLSelectElement, ['name']);
const HTMLFormElement = defineElement('HTMLFormElement', ['form'], class extends HTMLElement {
    get elements() { return new HTMLCollection(N.select(this, 'input,select,textarea,button', false)); }
    get length() { return this.elements.length; }
    /* sent by the engine, GET or POST, as its button would */
    submit() { N.submitForm(this); }
    requestSubmit(submitter) {
        if (this.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))) N.submitForm(this);
    }
    reset() {}
    checkValidity() { return true; } reportValidity() { return true; }
});
reflect(HTMLFormElement, ['name', 'method', 'target', 'enctype']);
reflectURL(HTMLFormElement, ['action']);
defineElement('HTMLVideoElement', ['video'], class extends HTMLElement {
    play() { return Promise.reject(new Error('no video yet')); } pause() {} load() {}
    canPlayType() { return ''; } get paused() { return true; }
});
defineElement('HTMLAudioElement', ['audio'], class extends HTMLElement {
    play() { return Promise.reject(new Error('no audio yet')); } pause() {} load() {}
    canPlayType() { return ''; } get paused() { return true; }
});
global.Audio = function () { return document.createElement('audio'); };

/* --- the document --- */

class DocumentFragment extends Node {
    constructor() { super(); return N.createFragment(); }
    getElementById(id) { return N.select(this, '#' + cssEscape(id), true)[0] || null; }
}
mixin(DocumentFragment, ParentNode);

function cssEscape(s) {
    return String(s).replace(/[^a-zA-Z0-9_ -￿-]/g, c => '\\' + c).replace(/^(\d)/, '\\3$1 ');
}

class Document extends Node {
    get documentElement() { return elementChildren(this).find(e => N.tag(e) === 'html') || elementChildren(this)[0] || null; }
    get head() { const h = this.documentElement; return h ? elementChildren(h).find(e => N.tag(e) === 'head') || null : null; }
    get body() { const h = this.documentElement; return h ? elementChildren(h).find(e => N.tag(e) === 'body') || null : null; }
    get title() { const t = N.select(this, 'title', true)[0]; return t ? N.text(t).trim() : ''; }
    set title(v) { let t = N.select(this, 'title', true)[0]; if (!t && this.head) { t = N.create('title'); N.append(this.head, t); } if (t) N.setText(t, String(v)); }
    get URL() { return N.url(); }
    get documentURI() { return N.url(); }
    get baseURI() { return N.url(); }
    get location() { return location; }
    set location(v) { location.href = v; }
    get domain() { return location.hostname; }
    set domain(v) {}
    get referrer() { return ''; }
    get cookie() { return N.cookie(); }
    set cookie(v) { N.setCookie(String(v)); }
    get readyState() { return readyState; }
    get compatMode() { return 'CSS1Compat'; }
    get characterSet() { return 'UTF-8'; }
    get charset() { return 'UTF-8'; }
    get inputEncoding() { return 'UTF-8'; }
    get contentType() { return 'text/html'; }
    get defaultView() { return global; }
    get hidden() { return false; }
    get visibilityState() { return 'visible'; }
    get webkitVisibilityState() { return 'visible'; }
    get activeElement() { return this._active || this.body; }
    get currentScript() { return N.currentScript(); }
    get forms() { return new HTMLCollection(N.select(this, 'form', false)); }
    get images() { return new HTMLCollection(N.select(this, 'img', false)); }
    get links() { return new HTMLCollection(N.select(this, 'a[href],area[href]', false)); }
    get scripts() { return new HTMLCollection(N.select(this, 'script', false)); }
    get styleSheets() { return []; }
    get fonts() { return { ready: Promise.resolve(), check: () => true, load: () => Promise.resolve([]), addEventListener() {}, forEach() {} }; }
    get implementation() { return { hasFeature: () => true, createHTMLDocument: () => document }; }
    get doctype() { return null; }
    get scrollingElement() { return this.documentElement; }
    get fullscreenElement() { return null; }
    get fullscreenEnabled() { return false; }
    get timeline() { return { currentTime: N.now() }; }
    hasFocus() { return true; }
    getElementById(id) { return N.select(this, '#' + cssEscape(id), true)[0] || null; }
    getElementsByName(name) { return new NodeList(all(this).filter(e => N.attr(e, 'name') === String(name))); }
    createElement(tag) { return N.create(String(tag).toLowerCase()); }
    createElementNS(ns, tag) { return N.create(String(tag).replace(/^.*:/, '').toLowerCase()); }
    createTextNode(text) { return N.createText(String(text)); }
    createComment(text) { return N.createComment(String(text)); }
    createDocumentFragment() { return N.createFragment(); }
    createEvent(kind) { return kind && /mouse/i.test(kind) ? new MouseEvent('') : new CustomEvent(''); }
    createRange() { return new Range(); }
    createTreeWalker(root) { const list = [root, ...all(root)]; let i = 0; return { currentNode: root, nextNode() { return (this.currentNode = list[++i] || null); } }; }
    createNodeIterator(root) { return this.createTreeWalker(root); }
    getSelection() { return global.getSelection(); }
    importNode(node, deep) { return N.clone(node, !!deep); }
    adoptNode(node) { return node; }
    elementFromPoint() { return this.body; }
    elementsFromPoint() { return [this.body]; }
    execCommand() { return false; }
    queryCommandSupported() { return false; }
    exitFullscreen() { return Promise.resolve(); }
    write(...html) { documentWrite(html.join('')); }
    writeln(...html) { documentWrite(html.join('') + '\n'); }
    open() { return this; }
    close() {}
}
mixin(Document, ParentNode);
const HTMLDocument = Document;

function documentWrite(html) {
    const script = N.currentScript();
    const box = N.create('div');
    N.setInner(box, html);
    const parent = script ? N.parent(script) : document.body;
    if (!parent) return;
    const next = script ? N.next(script) : null;
    for (const c of children(box)) { N.insertBefore(parent, c, next); inserted(c); }
}

class Range {
    constructor() { this.startContainer = document; this.startOffset = 0; this.endContainer = document; this.endOffset = 0; this.collapsed = true; }
    setStart(n, o) { this.startContainer = n; this.startOffset = o; } setEnd(n, o) { this.endContainer = n; this.endOffset = o; }
    setStartBefore() {} setStartAfter() {} setEndBefore() {} setEndAfter() {} selectNode() {} selectNodeContents() {} collapse() {}
    getBoundingClientRect() { return new DOMRect(0, 0, 0, 0); } getClientRects() { return []; }
    cloneRange() { return new Range(); } detach() {}
    createContextualFragment(html) { const box = N.create('div'); N.setInner(box, String(html)); const f = N.createFragment(); for (const c of children(box)) N.append(f, c); return f; }
    toString() { return ''; }
}

class DOMRect {
    constructor(x, y, w, h) { this.x = this.left = x || 0; this.y = this.top = y || 0; this.width = w || 0; this.height = h || 0; this.right = this.x + this.width; this.bottom = this.y + this.height; }
    toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom }; }
}

/* --- the prototype of each node as the engine makes its object --- */

function protoFor(type, tag) {
    switch (type) {
    case ELEMENT: return (tagClasses[tag] || (tag && tag.includes('-') ? HTMLElement : tagClasses[tag] || HTMLElement)).prototype;
    case TEXT: return Text.prototype;
    case COMMENT: return Comment.prototype;
    case DOCUMENT: return Document.prototype;
    case FRAGMENT: return DocumentFragment.prototype;
    }
    return Node.prototype;
}

N.setProtoFor(protoFor);
const document = N.document();
let readyState = 'loading';

/* --- URL --- */

class URLSearchParams {
    constructor(init) {
        this._list = [];
        if (init instanceof URLSearchParams) this._list = init._list.slice();
        else if (typeof init === 'string') {
            for (const part of init.replace(/^\?/, '').split('&')) {
                if (!part) continue;
                const i = part.indexOf('=');
                const dec = s => { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } };
                this._list.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]);
            }
        } else if (init && typeof init === 'object') {
            const entries = Symbol.iterator in init ? init : Object.entries(init);
            for (const [k, v] of entries) this._list.push([String(k), String(v)]);
        }
        this._url = null;
    }
    _changed() { if (this._url) this._url._search = this._list.length ? '?' + this : ''; }
    append(k, v) { this._list.push([String(k), String(v)]); this._changed(); }
    delete(k) { this._list = this._list.filter(e => e[0] !== String(k)); this._changed(); }
    get(k) { const e = this._list.find(e => e[0] === String(k)); return e ? e[1] : null; }
    getAll(k) { return this._list.filter(e => e[0] === String(k)).map(e => e[1]); }
    has(k) { return this._list.some(e => e[0] === String(k)); }
    set(k, v) { const i = this._list.findIndex(e => e[0] === String(k)); if (i < 0) this._list.push([String(k), String(v)]); else { this._list[i][1] = String(v); this._list = this._list.filter((e, j) => j <= i || e[0] !== String(k)); } this._changed(); }
    sort() { this._list.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); this._changed(); }
    forEach(f, self) { for (const [k, v] of this._list) f.call(self, v, k, this); }
    keys() { return this._list.map(e => e[0])[Symbol.iterator](); }
    values() { return this._list.map(e => e[1])[Symbol.iterator](); }
    entries() { return this._list.map(e => [e[0], e[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    get size() { return this._list.length; }
    toString() {
        const enc = s => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
        return this._list.map(([k, v]) => enc(k) + '=' + enc(v)).join('&');
    }
}

class URL {
    constructor(url, base) {
        const absolute = N.resolve(String(url), base === undefined ? (/^[a-z][a-z0-9+.-]*:/i.test(String(url)) ? String(url) : null) : String(base instanceof URL ? base.href : base));
        if (!absolute) throw new TypeError('Invalid URL: ' + url);
        this._parse(absolute);
    }
    _parse(href) {
        const m = /^([a-z][a-z0-9+.-]*:)(?:\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?([^:\/?#]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i.exec(href);
        if (!m) throw new TypeError('Invalid URL: ' + href);
        this._protocol = m[1].toLowerCase();
        this.username = m[2] || ''; this.password = m[3] || '';
        this._hostname = (m[4] || '').toLowerCase();
        this._port = m[5] || '';
        this._pathname = m[6] || (m[4] !== undefined ? '/' : '');
        this._search = m[7] && m[7] !== '?' ? m[7] : '';
        this._hash = m[8] && m[8] !== '#' ? m[8] : '';
        this._params = null;
    }
    get protocol() { return this._protocol; } set protocol(v) { this._protocol = String(v).replace(/:?$/, ':'); }
    get hostname() { return this._hostname; } set hostname(v) { this._hostname = String(v); }
    get port() { return this._port; } set port(v) { this._port = String(v); }
    get host() { return this._hostname + (this._port ? ':' + this._port : ''); }
    set host(v) { const [h, p] = String(v).split(':'); this._hostname = h; this._port = p || ''; }
    get pathname() { return this._pathname; } set pathname(v) { this._pathname = String(v).startsWith('/') ? String(v) : '/' + v; }
    get search() { return this._search; } set search(v) { v = String(v); this._search = v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''; this._params = null; }
    get hash() { return this._hash; } set hash(v) { v = String(v); this._hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
    get origin() { return /^(https?|ftp|wss?):$/.test(this._protocol) ? this._protocol + '//' + this.host : 'null'; }
    get searchParams() { if (!this._params) { this._params = new URLSearchParams(this._search); this._params._url = this; } return this._params; }
    get href() {
        const auth = this.username ? this.username + (this.password ? ':' + this.password : '') + '@' : '';
        const slashes = this._hostname || /^(https?|ftp|file|wss?):$/.test(this._protocol) ? '//' : '';
        return this._protocol + slashes + auth + this.host + this._pathname + this._search + this._hash;
    }
    set href(v) { this._parse(N.resolve(String(v)) || String(v)); }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static canParse(url, base) { try { new URL(url, base); return true; } catch (e) { return false; } }
    static createObjectURL() { return 'blob:' + location.origin + '/' + Math.random().toString(16).slice(2); }
    static revokeObjectURL() {}
}

/* --- location and history --- */

const location = {
    get href() { return N.url(); },
    set href(v) { N.navigate(String(v)); },
    assign(v) { N.navigate(String(v)); },
    replace(v) { N.navigate(String(v)); },
    reload() { N.navigate(N.url()); },
    toString() { return N.url(); },
    get ancestorOrigins() { return []; },
};
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin'])
    Object.defineProperty(location, part, {
        get() { return new URL(N.url())[part]; },
        set(v) {
            if (part === 'hash') { /* the same page: nothing to load */ return; }
            const u = new URL(N.url()); u[part] = v; N.navigate(u.href);
        },
    });

const historyStates = [{ state: null }];
const history = {
    get length() { return historyStates.length; },
    get state() { return historyStates[historyStates.length - 1].state; },
    get scrollRestoration() { return 'auto'; }, set scrollRestoration(v) {},
    pushState(state) { historyStates.push({ state }); },
    replaceState(state) { historyStates[historyStates.length - 1] = { state }; },
    back() {}, forward() {}, go() {},
};

/* --- timers, in virtual time: the page loads at once, whatever it waits for --- */

let timerId = 0, virtualNow = 0, live = false;
const timers = new Map();
/* the clock timers run by: virtual while the page loads, real once it shows */
function clock() { return live ? N.now() : virtualNow; }

function setTimeout(callback, delay, ...args) {
    const id = ++timerId;
    /* at least 4 ms, as browsers clamp them: a timer that sets itself again lets time go on */
    timers.set(id, { callback, args, due: clock() + Math.max(4, Number(delay) || 0), interval: 0 });
    return id;
}
function setInterval(callback, delay, ...args) {
    const id = ++timerId;
    const interval = Math.max(10, Number(delay) || 0);
    timers.set(id, { callback, args, due: clock() + interval, interval, count: 0 });
    return id;
}
function clearTimeout(id) { timers.delete(id); }

function runTimers() {
    if (live) {
        /* every timer that is due, a bounded number at a time */
        const now = N.now();
        for (let n = 0; n < 50; n++) {
            let first = null;
            for (const [id, t] of timers) if (t.due <= now && (!first || t.due < first[1].due)) first = [id, t];
            if (!first) break;
            const [id, t] = first;
            if (t.interval) t.due = now + t.interval; else timers.delete(id);
            try {
                if (typeof t.callback === 'function') t.callback.apply(global, t.args);
                else N.eval(String(t.callback), 'timer');
            } catch (e) { reportError(e); }
        }
        return nextTimer();
    }
    let first = null;
    for (const [id, t] of timers) if (!first || t.due < first[1].due) first = [id, t];
    if (!first) return -1;
    const [id, t] = first;
    /* what a page waits a second or more for - a timeout for an answer of another frame -
     * waits in real time once the page shows, or the answer could never come first */
    if (t.due > 1000) return -1;
    virtualNow = Math.max(virtualNow, t.due);
    /* an interval runs a few times while the page loads, not forever */
    if (t.interval && ++t.count < 5) t.due = virtualNow + t.interval; else timers.delete(id);
    try {
        if (typeof t.callback === 'function') t.callback.apply(global, t.args);
        else N.eval(String(t.callback), 'timer');
    } catch (e) { reportError(e); }
    return timers.size ? 0 : -1;
}

function nextTimer() {
    let due = Infinity;
    for (const t of timers.values()) due = Math.min(due, t.due);
    return due === Infinity ? -1 : Math.max(0, due - N.now());
}

let frameId = 0;
const frames = new Map();
function requestAnimationFrame(callback) {
    const id = ++frameId;
    /* an animation goes on forever: while the page loads, its first frames; then ten a second,
     * as each lays the page out again */
    if (!live && frameId > 30) return id;
    frames.set(id, setTimeout(() => { frames.delete(id); callback(performanceNow()); }, live ? 100 : 16));
    return id;
}
function cancelAnimationFrame(id) { clearTimeout(frames.get(id)); frames.delete(id); }

/* --- the network --- */

function headerMap(raw) {
    const map = new Map();
    if (raw) map.set('content-type', raw);
    return map;
}

class Headers {
    constructor(init) {
        this._map = new Map();
        if (init instanceof Headers) init.forEach((v, k) => this.set(k, v));
        else if (init) for (const [k, v] of (Symbol.iterator in init ? init : Object.entries(init))) this.append(k, v);
    }
    append(k, v) { k = String(k).toLowerCase(); this._map.set(k, this._map.has(k) ? this._map.get(k) + ', ' + v : String(v)); }
    set(k, v) { this._map.set(String(k).toLowerCase(), String(v)); }
    get(k) { const v = this._map.get(String(k).toLowerCase()); return v === undefined ? null : v; }
    has(k) { return this._map.has(String(k).toLowerCase()); }
    delete(k) { this._map.delete(String(k).toLowerCase()); }
    forEach(f, self) { for (const [k, v] of this._map) f.call(self, v, k, this); }
    entries() { return this._map.entries(); } keys() { return this._map.keys(); } values() { return this._map.values(); }
    [Symbol.iterator]() { return this._map.entries(); }
}

class Response {
    constructor(body, init) {
        init = init || {};
        this._text = body == null ? '' : String(body);
        this.status = init.status === undefined ? 200 : init.status;
        this.statusText = init.statusText || '';
        this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
        this.url = init.url || '';
        this.redirected = false;
        this.type = 'basic';
        this.bodyUsed = false;
    }
    get ok() { return this.status >= 200 && this.status < 300; }
    text() { this.bodyUsed = true; return Promise.resolve(this._text); }
    json() { this.bodyUsed = true; return new Promise((resolve, reject) => { try { resolve(JSON.parse(this._text)); } catch (e) { reject(e); } }); }
    arrayBuffer() { return Promise.resolve(this._bytes || new TextEncoder().encode(this._text).buffer); }
    blob() { return Promise.resolve(new Blob([this._text])); }
    clone() { const r = new Response(this._text, this); r._bytes = this._bytes; return r; }
}
class Request {
    constructor(input, init) {
        init = init || {};
        this.url = input instanceof Request ? input.url : (N.resolve(String(input)) || String(input));
        this.method = (init.method || (input instanceof Request ? input.method : 'GET')).toUpperCase();
        this.headers = new Headers(init.headers);
        this.body = init.body === undefined ? null : init.body;
        this.credentials = init.credentials || 'same-origin';
        this.signal = init.signal || null;
    }
}

/* what a script sends: bytes as they are, the rest as text */
function requestBody(body) {
    if (body == null) return undefined;
    if (body instanceof ArrayBuffer || ArrayBuffer.isView(body)) return body;
    if (body instanceof Blob) return body._text;
    if (body instanceof URLSearchParams || body instanceof FormData) return String(body);
    return String(body);
}

function fetch(input, init) {
    const request = new Request(input, init);
    return new Promise((resolve, reject) => {
        setTimeout(() => {
            if (request.signal && request.signal.aborted) return reject(new DOMException('aborted', 'AbortError'));
            let headers = '';
            request.headers.forEach((v, k) => { if (!/^(host|connection|content-length|cookie|user-agent|referer|origin)$/i.test(k)) headers += k + ': ' + v + '\r\n'; });
            const r = N.fetch(request.method, request.url, requestBody(request.body), headers || undefined, true);
            if (!r.status) return reject(new TypeError('Failed to fetch'));
            const response = new Response(r.text, { status: r.status, headers: headerMap(r.type), url: r.url });
            response._bytes = r.bytes;
            resolve(response);
        }, 0);
    });
}

class XMLHttpRequest extends EventTarget {
    constructor() {
        super();
        this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = '';
        this.responseType = ''; this.responseURL = ''; this.timeout = 0; this.withCredentials = false;
        this.upload = new EventTarget();
        this._type = '';
    }
    open(method, url, async) { this._method = String(method).toUpperCase(); this._url = N.resolve(String(url)) || String(url); this._async = async !== false; this.readyState = 1; this._headers = ''; }
    setRequestHeader(name, value) {
        if (/^(host|connection|content-length|cookie|user-agent|referer|origin|accept-encoding)$/i.test(name)) return;
        this._headers += name + ': ' + String(value).replace(/[\r\n]/g, ' ') + '\r\n';
    }
    overrideMimeType() {}
    getResponseHeader(name) { return /content-type/i.test(name) ? this._type : null; }
    getAllResponseHeaders() { return this._type ? 'content-type: ' + this._type + '\r\n' : ''; }
    abort() { this._aborted = true; }
    send(body) {
        const run = () => {
            if (this._aborted) return;
            const binary = this.responseType === 'arraybuffer' || this.responseType === 'blob';
            const r = N.fetch(this._method, this._url, requestBody(body), this._headers || undefined, binary);
            this.status = r.status; this.statusText = r.status === 200 ? 'OK' : ''; this.responseURL = r.url; this._type = r.type;
            this.responseText = binary ? '' : r.text;
            this.response = this.responseType === 'json' ? (() => { try { return JSON.parse(r.text); } catch (e) { return null; } })()
                          : this.responseType === 'arraybuffer' ? r.bytes
                          : this.responseType === 'blob' ? new Blob([r.text], { type: r.type }) : r.text;
            this.readyState = 4;
            const fire = type => { const e = new ProgressEvent(type); this.dispatchEvent(e); };
            fire('readystatechange');
            fire(r.status ? 'load' : 'error');
            fire('loadend');
        };
        if (this._async) setTimeout(run, 0); else run();
    }
}
Object.assign(XMLHttpRequest, { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 });

/* --- the rest of window --- */

class Blob {
    constructor(parts, options) { this._text = (parts || []).map(p => typeof p === 'string' ? p : '').join(''); this.type = (options && options.type) || ''; }
    get size() { return this._text.length; }
    text() { return Promise.resolve(this._text); }
    arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this._text).buffer); }
    slice() { return new Blob([this._text]); }
}
class File extends Blob { constructor(parts, name, options) { super(parts, options); this.name = name; this.lastModified = Date.now(); } }
class FormData {
    constructor(form) { this._list = []; if (form) for (const e of form.elements) if (e.name) this._list.push([e.name, e.value]); }
    append(k, v) { this._list.push([String(k), v]); } get(k) { const e = this._list.find(e => e[0] === k); return e ? e[1] : null; }
    getAll(k) { return this._list.filter(e => e[0] === k).map(e => e[1]); } has(k) { return this._list.some(e => e[0] === k); }
    set(k, v) { this.delete(k); this.append(k, v); } delete(k) { this._list = this._list.filter(e => e[0] !== k); }
    entries() { return this._list[Symbol.iterator](); } [Symbol.iterator]() { return this.entries(); }
    toString() { return new URLSearchParams(this._list.map(e => [e[0], String(e[1])])).toString(); }
}
class DOMException extends Error {
    constructor(message, name) { super(message); this.name = name || 'Error'; this.code = 0; }
}
class AbortSignal extends EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static timeout() { return new AbortSignal(); }
    static abort(reason) { const s = new AbortSignal(); s.aborted = true; s.reason = reason; return s; }
    static any() { return new AbortSignal(); }
}
class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason) { if (this.signal.aborted) return; this.signal.aborted = true; this.signal.reason = reason; this.signal.dispatchEvent(new Event('abort')); }
}
class Observer {
    constructor(callback) { this._callback = callback; }
    observe() {} unobserve() {} disconnect() {} takeRecords() { return []; }
}
class MutationObserver extends Observer {}
class ResizeObserver extends Observer {}
class PerformanceObserver extends Observer { static get supportedEntryTypes() { return []; } }
/* everything is on screen: what waits to be seen loads */
class IntersectionObserver extends Observer {
    observe(target) {
        setTimeout(() => {
            try { this._callback([{ target, isIntersecting: true, intersectionRatio: 1, boundingClientRect: new DOMRect(), intersectionRect: new DOMRect(), rootBounds: null, time: performanceNow() }], this); } catch (e) { reportError(e); }
        }, 0);
    }
}

class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s) {
        s = String(s === undefined ? '' : s);
        const out = [];
        for (const ch of s) {
            let c = ch.codePointAt(0);
            if (c < 0x80) out.push(c);
            else if (c < 0x800) out.push(0xc0 | c >> 6, 0x80 | c & 63);
            else if (c < 0x10000) out.push(0xe0 | c >> 12, 0x80 | c >> 6 & 63, 0x80 | c & 63);
            else out.push(0xf0 | c >> 18, 0x80 | c >> 12 & 63, 0x80 | c >> 6 & 63, 0x80 | c & 63);
        }
        return new Uint8Array(out);
    }
}
class TextDecoder {
    constructor(label) { this.encoding = (label || 'utf-8').toLowerCase(); }
    decode(buffer) {
        if (!buffer) return '';
        const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer.buffer || buffer);
        let out = '', i = 0;
        while (i < bytes.length) {
            const b = bytes[i++];
            let c;
            if (b < 0x80) c = b;
            else if (b < 0xe0) c = (b & 31) << 6 | bytes[i++] & 63;
            else if (b < 0xf0) c = (b & 15) << 12 | (bytes[i++] & 63) << 6 | bytes[i++] & 63;
            else c = (b & 7) << 18 | (bytes[i++] & 63) << 12 | (bytes[i++] & 63) << 6 | bytes[i++] & 63;
            out += String.fromCodePoint(c);
        }
        return out;
    }
}

const base64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function btoa(s) {
    s = String(s);
    let out = '';
    for (let i = 0; i < s.length; i += 3) {
        const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
        if (a > 255 || b > 255 || c > 255) throw new DOMException('not Latin-1', 'InvalidCharacterError');
        out += base64[a >> 2] + base64[(a & 3) << 4 | (b >> 4 || 0)] +
               (i + 1 < s.length ? base64[(b & 15) << 2 | (c >> 6 || 0)] : '=') + (i + 2 < s.length ? base64[c & 63] : '=');
    }
    return out;
}
function atob(s) {
    s = String(s).replace(/[\s=]/g, '');
    let out = '', bits = 0, value = 0;
    for (const ch of s) {
        const v = base64.indexOf(ch);
        if (v < 0) throw new DOMException('not base64', 'InvalidCharacterError');
        value = value << 6 | v; bits += 6;
        if (bits >= 8) { bits -= 8; out += String.fromCharCode(value >> bits & 255); }
    }
    return out;
}

const start = N.now(), timeOrigin = Date.now();
function performanceNow() { return N.now() - start; }
const performance = {
    now: performanceNow,
    timeOrigin,
    timing: { navigationStart: timeOrigin, fetchStart: timeOrigin, responseStart: timeOrigin, responseEnd: timeOrigin,
              domLoading: timeOrigin, domInteractive: timeOrigin, domContentLoadedEventEnd: timeOrigin, loadEventEnd: 0 },
    navigation: { type: 0, redirectCount: 0 },
    getEntries() { return []; }, getEntriesByType() { return []; }, getEntriesByName() { return []; },
    mark() {}, measure() {}, clearMarks() {}, clearMeasures() {}, setResourceTimingBufferSize() {},
    addEventListener() {}, removeEventListener() {},
    toJSON() { return {}; },
};

function storage() {
    const data = new Map();
    const target = {
        getItem(k) { return data.has(String(k)) ? data.get(String(k)) : null; },
        setItem(k, v) { data.set(String(k), String(v)); },
        removeItem(k) { data.delete(String(k)); },
        clear() { data.clear(); },
        key(i) { return [...data.keys()][i] ?? null; },
        get length() { return data.size; },
    };
    return new Proxy(target, {
        get(t, p) { return p in t ? t[p] : (typeof p === 'string' && data.has(p) ? data.get(p) : undefined); },
        set(t, p, v) { if (p in t) return false; data.set(String(p), String(v)); return true; },
    });
}

const navigator = {
    userAgent: N.userAgent,
    appVersion: N.userAgent.replace(/^Mozilla\//, ''),
    appName: 'Netscape', appCodeName: 'Mozilla', product: 'Gecko', productSub: '20030107',
    vendor: '', vendorSub: '',
    platform: 'Win32',
    language: 'uk-UA', languages: ['uk-UA', 'uk', 'en-US', 'en'],
    onLine: true, cookieEnabled: true, doNotTrack: null, webdriver: false, pdfViewerEnabled: false,
    hardwareConcurrency: 4, deviceMemory: 8, maxTouchPoints: 0,
    plugins: Object.assign([], { item: () => null, namedItem: () => null, refresh() {} }),
    mimeTypes: Object.assign([], { item: () => null, namedItem: () => null }),
    connection: { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener() {} },
    javaEnabled() { return false; },
    sendBeacon() { return true; },
    vibrate() { return false; },
    getBattery() { return Promise.resolve({ level: 1, charging: true, addEventListener() {} }); },
    clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve('') },
    permissions: { query: () => Promise.resolve({ state: 'prompt', addEventListener() {} }) },
    mediaDevices: { enumerateDevices: () => Promise.resolve([]), addEventListener() {} },
    serviceWorker: undefined,
    storage: { estimate: () => Promise.resolve({ quota: 1e9, usage: 0 }), persisted: () => Promise.resolve(false) },
    userAgentData: undefined,
    registerProtocolHandler() {},
};

const screen = { width: 1920, height: 1080, availWidth: 1920, availHeight: 1040, availLeft: 0, availTop: 0,
                 colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0, addEventListener() {} } };

function matchMedia(query) {
    const q = String(query);
    let matches = false;
    const width = N.width;
    let m;
    if ((m = /min-width:\s*([\d.]+)px/.exec(q))) matches = width >= +m[1];
    else if ((m = /max-width:\s*([\d.]+)px/.exec(q))) matches = width <= +m[1];
    else if (/prefers-color-scheme:\s*light|hover:\s*hover|pointer:\s*fine|screen|all/.test(q)) matches = true;
    return { matches, media: q, onchange: null, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} };
}

function getComputedStyle(element) {
    const style = styleProxy(element instanceof Element ? element : null);
    return new Proxy(style, {
        get(t, p) {
            if (p === 'display') return element && N.attr(element, 'hidden') !== null ? 'none' : 'block';
            if (p === 'visibility') return 'visible';
            if (p === 'getPropertyValue') return name => name === 'display' ? 'block' : '';
            return Reflect.get(t, p);
        },
    });
}

const crypto = {
    getRandomValues(array) { for (let i = 0; i < array.length; i++) array[i] = Math.floor(Math.random() * 4294967296); return array; },
    randomUUID() { return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => { const r = Math.random() * 16 | 0; return (c === 'x' ? r : r & 3 | 8).toString(16); }); },
    subtle: {},
};

const console = {};
for (const level of ['log', 'info', 'warn', 'error', 'debug', 'trace', 'dir', 'table', 'group', 'groupCollapsed', 'groupEnd', 'time', 'timeEnd', 'timeLog', 'count', 'assert'])
    console[level] = (...args) => N.log(level + ': ' + args.map(a => { try { return typeof a === 'string' ? a : JSON.stringify(a); } catch (e) { return String(a); } }).join(' '));

const customElements = {
    _defined: new Map(),
    define(name, cls) { this._defined.set(name, cls); tagClasses[name] = cls; },
    get(name) { return this._defined.get(name); },
    whenDefined(name) { return Promise.resolve(this._defined.get(name)); },
    upgrade() {},
};

class Image extends HTMLElement { constructor(w, h) { super(); const img = N.create('img'); if (w) img.width = w; if (h) img.height = h; return img; } }
class Option extends HTMLElement { constructor(text, value) { super(); const o = N.create('option'); if (text) N.setText(o, String(text)); if (value !== undefined) N.setAttr(o, 'value', String(value)); return o; } }
class DOMParser {
    parseFromString(text) { const box = N.create('div'); N.setInner(box, String(text)); return { body: box, documentElement: box, querySelector: s => N.select(box, s, true)[0] || null, querySelectorAll: s => new NodeList(N.select(box, s, false)), getElementsByTagName: t => box.getElementsByTagName(t) }; }
}
class XMLSerializer { serializeToString(node) { return N.outer(node); } }
/* two ports of one window: what one posts, the other gets as a message, in a later task */
/* the ports another window holds the other end of: by the id the engine gave them */
const portsById = new Map();
class MessagePort extends EventTarget {
    constructor() { super(); this._other = null; this._closed = false; this.onmessage = null; this.onmessageerror = null; }
    postMessage(data) {
        const other = this._other;
        if (this._closed) return;
        /* the other end is in another window: the engine carries it there */
        if (this._remote || (other && other._transferred)) {
            if (this._id) N.portPost(this._id, JSON.stringify(data === undefined ? null : data));
            return;
        }
        if (!other) return;
        setTimeout(() => {
            if (other._closed) return;
            const event = new MessageEvent('message', { data, origin: '' });
            event.ports = [];
            other.dispatchEvent(event);
        }, 0);
    }
    start() {} close() { this._closed = true; }
}
class MessageChannel {
    constructor() { this.port1 = new MessagePort(); this.port2 = new MessagePort(); this.port1._other = this.port2; this.port2._other = this.port1; }
}
class BroadcastChannel extends EventTarget { constructor(name) { super(); this.name = name; } postMessage() {} close() {} }
class Worker extends EventTarget { constructor() { super(); } postMessage() {} terminate() {} }
class WebSocket extends EventTarget {
    constructor(url) { super(); this.url = url; this.readyState = 3; setTimeout(() => { this.dispatchEvent(new Event('error')); this.dispatchEvent(new Event('close')); }, 0); }
    send() {} close() {}
}

function getSelection() {
    return { rangeCount: 0, isCollapsed: true, toString: () => '', removeAllRanges() {}, addRange() {}, getRangeAt: () => new Range(), collapse() {}, selectAllChildren() {} };
}

/* --- window: the global object --- */

const window = global;
/* in a frame, the page it is in is another window: reached by postMessage; its frames by name */
const parentWindow = N.isFrame() ? remoteWindow((json, origin, ports) => N.postParent(json, origin, ports)) : window;
if (parentWindow !== window) {
    const siblings = new Map();
    parentWindow.frames = new Proxy({}, {
        get(t, name) {
            if (typeof name !== 'string' || name === 'then') return undefined;
            if (!siblings.has(name)) siblings.set(name, remoteWindow((json, origin, ports) => N.postSibling(name, json, origin, ports)));
            return siblings.get(name);
        },
    });
}
/* this window's frames by name, as frames[name] and window[name] find them */
const ownFrames = new Proxy(global, {
    get(t, name) {
        if (typeof name === 'string' && !(name in t)) {
            const frame = N.select(document, 'iframe[name="' + name.replace(/["\\]/g, '\\$&') + '"]', true)[0];
            if (frame) return frame.contentWindow;
        }
        return Reflect.get(t, name);
    },
});
const definitions = {
    window, self: window, globalThis: window, top: parentWindow, parent: parentWindow, frames: ownFrames, opener: null,
    frameElement: null, closed: false, length: 0, name: N.frameName(), status: '',
    document, location, history, navigator, screen, performance, crypto, console, customElements,
    localStorage: storage(), sessionStorage: storage(),
    innerWidth: N.width, innerHeight: N.height, outerWidth: N.width, outerHeight: N.height + 80,
    devicePixelRatio: 1, scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0, screenX: 0, screenY: 0,
    screenLeft: 0, screenTop: 0, isSecureContext: /^https:/.test(N.url()), visualViewport: null, origin: N.origin(),
    setTimeout, setInterval, clearTimeout, clearInterval: clearTimeout, requestAnimationFrame, cancelAnimationFrame,
    requestIdleCallback: callback => setTimeout(() => callback({ didTimeout: false, timeRemaining: () => 50 }), 1),
    cancelIdleCallback: clearTimeout,
    queueMicrotask: callback => { NativePromise.resolve().then(callback); },
    setImmediate: callback => setTimeout(callback, 0),
    fetch, atob, btoa, matchMedia, getComputedStyle, getSelection,
    alert() {}, confirm() { return false; }, prompt() { return null; }, print() {},
    open() { return null; }, close() {}, stop() {}, focus() {}, blur() {}, postMessage() {}, moveTo() {}, resizeTo() {},
    scroll() {}, scrollTo() {}, scrollBy() {},
    reportError, structuredClone: value => JSON.parse(JSON.stringify(value)),
    EventTarget, Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, KeyboardEvent, FocusEvent, InputEvent, ErrorEvent,
    MessageEvent, PopStateEvent, ProgressEvent, PromiseRejectionEvent, Node, Element, HTMLElement, CharacterData, Text, Comment, Document,
    HTMLDocument, DocumentFragment, NodeList, HTMLCollection, DOMTokenList, CSSStyleDeclaration, DOMRect, Range,
    URL, URLSearchParams, Headers, Request, Response, XMLHttpRequest, Blob, File, FormData, DOMException,
    AbortController, AbortSignal, MutationObserver, ResizeObserver, IntersectionObserver, PerformanceObserver,
    TextEncoder, TextDecoder, Image, Option, DOMParser, XMLSerializer, MessageChannel, MessagePort, BroadcastChannel, Worker,
    WebSocket, Window: function Window() {}, CSS: { supports: () => false, escape: cssEscape },
    SVGElement: class SVGElement extends Element {}, ShadowRoot: DocumentFragment,
    HTMLSlotElement: HTMLElement,
};
/* the handlers of window's events, there before a page sets one */
for (const type of ['load', 'unload', 'beforeunload', 'error', 'resize', 'scroll', 'message', 'popstate', 'hashchange',
                    'focus', 'blur', 'online', 'offline', 'pageshow', 'pagehide', 'storage', 'click', 'keydown',
                    'keyup', 'mousedown', 'mouseup', 'mousemove', 'touchstart', 'unhandledrejection', 'visibilitychange'])
    definitions['on' + type] = null;
for (const [k, v] of Object.entries(definitions)) {
    try { Object.defineProperty(global, k, { value: v, writable: true, configurable: true, enumerable: true }); } catch (e) {}
}
for (const [name, cls] of Object.entries(classes)) global[name] = cls;
/* The platform's functions are the browser's own: to the page they are
 * native code, as in every browser - not the JavaScript they are written in
 * here. */
const platform = new WeakSet();
function markPlatform(object) {
    if (!object) return;
    for (const key of Object.getOwnPropertyNames(object)) {
        const d = Object.getOwnPropertyDescriptor(object, key);
        if (!d) continue;
        for (const f of [d.value, d.get, d.set]) if (typeof f === 'function') platform.add(f);
    }
}
for (const cls of [EventTarget, Event, CustomEvent, UIEvent, MouseEvent, PointerEvent, KeyboardEvent, FocusEvent,
                   InputEvent, ErrorEvent, MessageEvent, PopStateEvent, ProgressEvent, Node, CharacterData, Text,
                   Comment, Element, HTMLElement, Document, DocumentFragment, NodeList, HTMLCollection, DOMTokenList,
                   CSSStyleDeclaration, DOMRect, Range, URL, URLSearchParams, Headers, Request, Response,
                   XMLHttpRequest, Blob, File, FormData, DOMException, AbortController, AbortSignal, MutationObserver,
                   ResizeObserver, IntersectionObserver, PerformanceObserver, TextEncoder, TextDecoder, Image, Option,
                   DOMParser, XMLSerializer, MessageChannel, MessagePort, BroadcastChannel, Worker, WebSocket, ...Object.values(classes)]) {
    platform.add(cls);
    markPlatform(cls);
    markPlatform(cls.prototype);
}
for (const object of [location, history, navigator, screen, performance, crypto, console, customElements, Object.getPrototypeOf(location)])
    markPlatform(object);
for (const v of Object.values(definitions)) if (typeof v === 'function') platform.add(v);
const functionToString = Function.prototype.toString;
const nativeToString = function toString() {
    if (platform.has(this) || this === nativeToString) return 'function ' + (this.name || '') + '() { [native code] }';
    return functionToString.call(this);
};
Object.defineProperty(Function.prototype, 'toString', { value: nativeToString, writable: true, configurable: true });

/* the global object keeps its prototype: window's events are its own properties */
for (const m of ['addEventListener', 'removeEventListener', 'dispatchEvent'])
    Object.defineProperty(global, m, { value: EventTarget.prototype[m], writable: true, configurable: true });

/* <script type=importmap>: what a bare name of an import stands for */
const importMap = new Map();
function addImportMap(text, base) {
    let map;
    try { map = JSON.parse(text); } catch (e) { return; }
    for (const [name, target] of Object.entries((map && map.imports) || {}))
        importMap.set(name, N.resolve(String(target), base) || String(target));
}
function resolveImport(name) {
    if (importMap.has(name)) return importMap.get(name);
    let best = null;
    for (const [prefix, target] of importMap)
        if (prefix.endsWith('/') && name.startsWith(prefix) && (!best || prefix.length > best[0].length)) best = [prefix, target];
    return best ? best[1] + name.slice(best[0].length) : null;
}

/* an event of the user: a mouse button, a click, a key, an edit of a field */
function dispatch(target, type, x, y) {
    let event;
    if (/^(mouse|click|dblclick|contextmenu)/.test(type)) {
        event = new MouseEvent(type, { bubbles: true, cancelable: true, clientX: x, clientY: y, button: 0, buttons: type === 'mousedown' ? 1 : 0 });
        if (type.startsWith('mouse')) {
            /* pointer events come with the mouse's on today's pages */
            const p = new PointerEvent(type.replace('mouse', 'pointer'), { bubbles: true, cancelable: true, clientX: x, clientY: y });
            p.isTrusted = true;
            target.dispatchEvent(p);
        }
    } else if (type === 'focus' || type === 'blur') event = new FocusEvent(type, {});
    else if (type === 'keydown' || type === 'keyup' || type === 'keypress') event = new KeyboardEvent(type, { bubbles: true, cancelable: true, key: String.fromCharCode(x), keyCode: x });
    else event = new Event(type, { bubbles: true, cancelable: type === 'submit' || type === 'click' });
    event.isTrusted = true;
    if (type === 'focus') document._active = target;
    return !target.dispatchEvent(event);
}

/* postMessage to itself goes round the loop like any other */
definitions.postMessage = function postMessage(data, targetOrigin, transfer) {
    const json = JSON.stringify(data === undefined ? null : data);
    if (targetOrigin && typeof targetOrigin === 'object') transfer = targetOrigin.transfer;
    const ports = (transfer || []).filter(p => p instanceof MessagePort);
    setTimeout(() => {
        let data;
        try { data = JSON.parse(json); } catch (e) { data = json; }
        const event = new MessageEvent('message', { data, origin: N.origin() });
        event.source = window;
        event.ports = ports;
        event.isTrusted = true;
        window.dispatchEvent(event);
    }, 0);
};
Object.defineProperty(global, 'postMessage', { value: definitions.postMessage, writable: true, configurable: true });

function deliverMessage(json, origin, sourceFrame, portIds, toPort) {
    let data;
    try { data = JSON.parse(json); } catch (e) { data = json; }
    if (toPort) {
        /* by a port: to the port here whose other end sent it */
        const port = portsById.get(toPort);
        if (port) {
            const event = new MessageEvent('message', { data, origin: '' });
            event.ports = [];
            event.isTrusted = true;
            port.dispatchEvent(event);
        }
        return;
    }
    const ports = (portIds || []).map(id => {
        const port = new MessagePort();
        port._id = id;
        port._remote = true;
        portsById.set(id, port);
        return port;
    });
    const event = new MessageEvent('message', { data, origin });
    event.source = sourceFrame ? sourceFrame.contentWindow : parentWindow;
    event.ports = ports;
    event.isTrusted = true;
    window.dispatchEvent(event);
}

return {
    protoFor,
    runTimers,
    nextTimer,
    dispatch,
    deliverMessage,
    goLive() { live = true; for (const t of timers.values()) t.due = N.now() + Math.max(0, t.due - virtualNow); },
    addImportMap,
    resolveImport,
    fireLoad() {
        readyState = 'interactive';
        document.dispatchEvent(new Event('readystatechange'));
        document.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));
        readyState = 'complete';
        document.dispatchEvent(new Event('readystatechange'));
        window.dispatchEvent(new Event('load'));
        window.dispatchEvent(new Event('pageshow'));
    },
};
})
