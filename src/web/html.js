/* html.js — escape untrusted text before it is interpolated into HTML markup.
 *
 * The archive list is rendered as one HTML string built from run records, and
 * those records come from localStorage and from imported archive files: every
 * string field in them ("fw", "rule", "ts", ...) is untrusted and must render as
 * text, never as markup or an attribute break-out. Pure and DOM-free so that
 * `npm run test:web` covers it.
 *
 * String(v), not `v ?? ''`: the rows used to interpolate raw values, and a
 * missing field must keep reading "undefined" exactly as it did before. */
const HTML_ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };

export function escapeHtml(v) {
    return String(v).replace(/[&<>"']/g, c => HTML_ESCAPES[c]);
}
