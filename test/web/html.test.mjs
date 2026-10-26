import { test } from 'node:test';
import assert from 'node:assert/strict';
import { escapeHtml } from '../../src/web/html.js';

/* escapeHtml guards the archive list, which is built as an HTML string from
 * run records. Those records come from localStorage and from imported archive
 * files, so every string field is untrusted: a crafted "fw" or "ts" must render
 * as text, never as markup or as an attribute break-out. */

test('markup is rendered inert', () => {
    assert.equal(escapeHtml('<img src=x onerror=alert(1)>'),
                 '&lt;img src=x onerror=alert(1)&gt;');
});

test('quotes cannot break out of a double- or single-quoted attribute', () => {
    assert.equal(escapeHtml('" onmouseover="x'), '&quot; onmouseover=&quot;x');
    assert.equal(escapeHtml("' onmouseover='x"), '&#39; onmouseover=&#39;x');
});

test('ampersand is escaped first, so existing entities are not decoded', () => {
    assert.equal(escapeHtml('&lt;b&gt;'), '&amp;lt;b&amp;gt;');
});

test('non-strings stringify exactly like a template literal did', () => {
    // The rows previously interpolated raw values; keep their visible text.
    assert.equal(escapeHtml(42), '42');
    assert.equal(escapeHtml(undefined), 'undefined');
    assert.equal(escapeHtml(null), 'null');
});

test('ordinary archive text passes through unchanged', () => {
    const s = 'dry · 55° · 4.0h · v1.2.3-4-gabc → 180°';
    assert.equal(escapeHtml(s), s);
});
