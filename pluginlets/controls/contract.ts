// contract.ts — the SHARED contract of the `controls` pluginlet (doc 37): the
// default plugin UI. The single source of truth for the `$schema` UI tree the
// native half emits (pluginlets/controls/controls.hpp) and the generic webui
// renderer consumes. Both sides validate against this; documented nowhere else.
//
// Widget vocabulary is THIS pluginlet's contract, NOT core ABI — a new widget /
// container type is a version bump here, the frozen ABI never moves (doc 37).

/** The three operations, all riding the host's EXISTING instance-control API —
 *  no new transport, no ABI change (doc 37):
 *   - describe : GET the schema      → get_def().$schema
 *   - read     : GET current values  → get_def()  (flat keys)
 *   - write    : set a param         → set_def({key: value})   (persists)
 *   - invoke   : press a button      → exchange({command})     (transient)
 */

/** What get_def() returns. Values are FLAT top-level keys; `$schema` is the tree.
 *  `$rev` bumps only when the STRUCTURE changes, so the renderer caches the tree
 *  and patches values in place when `$rev` is unchanged (no churn / lost focus). */
export interface ControlsDef {
  [valueKey: string]: unknown; // flat current values (fps, gain, mode, …)
  $v: number;                  // schema version (per-plet)
  $rev: number;                // structure revision
  $schema: Node;               // the UI tree (root container)
}

/** A tree node is a CONTAINER (has children) or a CONTROL leaf. */
export type Node = Container | Control;

export interface Container {
  type: "root" | "tab" | "section" | "row" | "group";
  title?: string;
  collapsed?: boolean;         // DEFAULT collapse only; the operator's actual
                               // open/closed state is browser-local, never in def
  children: Node[];
}

export type Widget =
  // --- value-bearing (bound to a def key) ---
  | "slider"    // numeric, min/max, drag
  | "numpad"    // numeric, min/max, TOUCH keypad entry (host-owned IME)
  | "toggle"    // boolean
  | "dropdown"  // enum, options[] — pick one, collapsed
  | "radio"     // enum, options[] — pick one, all shown (same data as dropdown)
  | "text"      // string
  // --- action ---
  | "button"    // fires `command` via exchange — NOT a bound value
  // --- output (plugin-pushed, read-only) ---
  | "readout"   // read-only output the plugin pushes; not writable via set_def
  // --- presentation-only (no value, no key) ---
  | "title"     // a section heading (text in `label`)
  | "label"     // a paragraph of help / body text (text in `label`)
  | "divider";  // a horizontal rule

export interface Control {
  type: "control";
  widget: Widget;
  key?: string;                // value/readout binding key (absent for button/title/label/divider)
  command?: string;            // button only: the exchange command to fire
  label?: string;              // control label, or the text of a title/label leaf
  min?: number;                // slider / numpad
  max?: number;
  options?: string[];          // dropdown / radio
}

/** Validation is DECLARED once (min/max/options) and enforced on BOTH sides: the
 *  native set_def clamps/rejects (never trusts the client); the UI mirrors it for
 *  good UX. A numpad commit should sanitize (clamp/round) to [min,max] — the same
 *  bound the native side re-applies. */
