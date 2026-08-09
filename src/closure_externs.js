/**
 * Closure Compiler externs for the EdgeDepth WASM client (Release build).
 *
 * --compilation_level ADVANCED_OPTIMIZATIONS renames every property/global it
 * doesn't recognise. Anything our hand-written JS (shell.html,
 * coi-serviceworker.js) or our EM_ASM / EM_ASM_INT blocks reference by name
 * must be declared here or it gets mangled / flagged as undeclared.
 *
 * Emscripten passes its own closure-externs.js too (standard DOM/WebGL/window
 * surface), but it does NOT cover:
 *   - localStorage  (used by the SymbolRegistry metadata cache EM_ASM blocks)
 *   - our custom Module.* members
 *
 * Closure externs are a separate compilation scope from the generated JS, so
 * declaring `var Module` here does NOT collide with the `var Module` in the
 * compiled input - it tells Closure "this global exists, don't rename its
 * members". Keep in sync with shell.html and any new EM_ASM references.
 */

/**
 * Browser localStorage - referenced directly inside EM_ASM blocks in
 * symbol_metadata.cpp (metadata cache read/write). Not in Emscripten's
 * default externs, so declare it to avoid JSC_UNDEFINED_VARIABLE.
 * @type {!Storage}
 */
var localStorage;

/**
 * The Emscripten Module object. Declared so its custom members below are
 * never renamed by advanced optimisation.
 * @type {!Object}
 * @suppress {duplicate}
 */
var Module = {};

/**
 * The SDL/WebGL canvas element. Set by shell.html, read by EM_ASM blocks in
 * main_loop() (canvas.width / canvas.height on resize).
 * @type {!HTMLCanvasElement}
 */
Module.canvas;

/**
 * Runtime-ready hook invoked by shell.html once the WASM module initialises.
 * @type {function()}
 */
Module.onRuntimeInitialized;

/**
 * Loading-status callback used by the shell's progress UI.
 * @type {function(string)}
 */
Module.setStatus;
