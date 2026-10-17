#lang racket/base
;; The checked program: what the parser produces and the backends consume. It describes the
;; interface (types, bindings, stages) without assuming a backend, so adding a backend only adds an
;; emitter.

(require racket/list
         racket/match
         racket/path
         racket/string)

(provide (all-defined-out))

;; Types
(struct scalar-t (kind) #:transparent)          ; 'float 'int 'uint 'bool, or a small integer kind
(struct vector-t (kind size) #:transparent)     ; kind of the components, 2-4 of them
(struct matrix-t (size) #:transparent)          ; square float matrices, 2-4 columns
(struct array-t (element length) #:transparent) ; fixed length
(struct struct-t (decl) #:transparent)          ; a struct-decl

;; Small integers ('uint8 'int8 'uint16 'int16) are storage types: buffers and shared memory hold
;; them, and shader bodies widen them to 32 bits when they read them (see body.rkt)
(define small-integer-kinds '(uint8 int8 uint16 int16))
(define (small-integer-kind? k) (and (memq k small-integer-kinds) #t))
(define (small-integer-bits k) (case k [(uint8 int8) 8] [(uint16 int16) 16]))
;; The 32-bit kind a small integer widens to
(define (widened-kind k) (case k [(uint8 uint16) 'uint] [(int8 int16) 'int] [else k]))

;; Types only shader bodies have: opaque resources ('texture2D 'sampler 'image2D), the unsized
;; array a buffer-array binding holds, nothing ('void, for effects and functions returning
;; nothing), and several values (from a fold with several accumulators)
(struct opaque-t (kind) #:transparent)
(struct runtime-array-t (element) #:transparent)
;; An array binding: its elements' type (a texture, storage image, buffer's value or buffer-array's
;; array), chosen with `at`
(struct binding-array-t (element) #:transparent)
(struct void-t () #:transparent)
(struct values-t (types) #:transparent)

;; Declarations. Every `loc` is where the declaration is in the .zgal file.
(struct field (name type loc) #:transparent)
(struct struct-decl (name fields loc) #:transparent)
(struct vertex-format (name attributes loc) #:transparent)   ; attributes: fields

;; path: the shader body's path (relative to the working directory if the .zgal path was).
;; name: its file name, which the shader library finds it by. stage: 'vertex 'fragment 'compute.
(struct shader (path name stage loc) #:transparent)

;; kind: 'texture 'sampler 'storage-image 'buffer 'buffer-array, or 'array: an array of entries
;; element: the type a buffer holds (or holds an array of), an array's element (a binding whose
;; kind is one of the others, but sampler), else #f
;; access: 'read 'write 'read-write, or #f for textures and samplers
;; stages: the stages using it (a subset of the material's), or '() for all of them
(struct binding (name kind element access stages loc) #:transparent)

;; attributes: the format's attributes read, as (cons index field); the index is the location
(struct vertex-input (format attributes loc) #:transparent)

;; interpolation: 'smooth 'flat 'noperspective
(struct varying (name type interpolation loc) #:transparent)

;; kind: 'graphics or 'compute. shaders: list of shader. push: fields. vertex-input: or #f.
;; varyings, outputs: lists (graphics only). workgroup: (list x y z) for compute, else #f.
;; shared: the compute shader's workgroup shared variables (fields).
(struct material (kind name shaders bindings push vertex-input varyings outputs workgroup shared loc)
  #:transparent)

;; A shader whose body is written in ZGAL (shader-path is #f). body: the checked body, a typed
;; expression (see body.rkt); uses: the functions and constants it uses, in dependency order.
(struct zgal-shader shader (body uses) #:transparent)

;; Top-level functions and constants. params: (cons name type). body: a typed expression.
;; cpu?: whether it can also be C++ (pure, without opaque types or stage built-ins). uses: the
;; functions and constants it uses, in dependency order.
(struct function-decl (name params result body cpu? uses loc) #:transparent)
(struct constant-decl (name type body cpu? uses loc) #:transparent)
;; A function written in GLSL, in the file at path (as given, relative to the working directory),
;; which shaders include. It has no body (#f), and no C++ version.
(struct glsl-function-decl function-decl (path) #:transparent)

;; source: the .zgal file's path. Declarations are in file order.
;; imports: the paths of the files it imports (directly or not), as their declarations' locs name them.
(struct program (source structs vertex-formats materials graphs functions apps imports) #:transparent)

;; ---------------------------------------------------------------------------------------------
;; Apps: state, the handlers changing it, and the graph they run. Bodies are typed expressions
;; (body.rkt), compiled for the CPU.

;; parameters: fields C++ gives. externs: function-decls (without bodies) of the functions C++ gives,
;; which bodies call. state: a struct-decl (named State) and the fields' initial values, in order.
;; handlers: list of handler. run: a graph-run, or #f.
(struct app (name parameters externs state initial handlers run loc) #:transparent)
;; event: kind 'key-pressed 'key-released (keys: key name symbols), 'mouse-pressed 'mouse-released
;; (button: 'left 'right 'middle), 'cursor-moved 'scrolled 'frame. binding: the name the event's
;; value is bound to (a cursor position, scroll offset or time step), or #f. body: the next state.
(struct event (kind keys button binding loc) #:transparent)
(struct handler (event body loc) #:transparent)
;; bindings: (cons graph-item-name expression) for the graph's parameters (computed once) and
;; per-frame inputs (each frame). enables: (cons node-name expression), computed each frame.
(struct graph-run (graph bindings enables loc) #:transparent)


;; The C++ namespace of prog's generated code: its file's name without .zgal (scene.zgal: scene)
(define (program-namespace prog)
  (source-namespace (program-source prog)))
(define (source-namespace source)
  (define name (let-values ([(_dir file _must-be-dir) (split-path source)]) (path->string file)))
  (regexp-replace #px"[.]zgal$" name ""))

;; The files of prog's GLSL functions, each once
(define (program-glsl-files prog)
  (remove-duplicates
   (for/list ([d (in-list (program-functions prog))] #:when (glsl-function-decl? d)) (glsl-function-decl-path d))))

(define (type-name t)
  (match t
    [(scalar-t k) (symbol->string k)]
    [(vector-t k n) (format "~a~a" (case k [(float) "vec"] [(int) "ivec"] [(uint) "uvec"] [(bool) "bvec"]) n)]
    [(matrix-t n) (format "mat~a" n)]
    [(array-t e n) (format "(array ~a ~a)" (type-name e) n)]
    [(struct-t d) (struct-decl-name d)]
    [(opaque-t k) (symbol->string k)]
    [(runtime-array-t e) (format "(array ~a)" (type-name e))]
    [(binding-array-t e) (format "an array of ~a" (type-name e))]
    [(void-t) "void"]
    [(values-t ts) (format "(values ~a)" (string-join (map type-name ts) " "))]))

;; The kind of b's resources: its kind, or for an array its elements'
(define (binding-resource-kind b)
  (if (eq? (binding-kind b) 'array) (binding-kind (binding-element b)) (binding-kind b)))

;; The type b's buffers hold (or hold arrays of), for buffer kinds and arrays of them, else #f
(define (binding-buffer-type b)
  (and (memq (binding-resource-kind b) '(buffer buffer-array))
       (if (eq? (binding-kind b) 'array) (binding-element (binding-element b)) (binding-element b))))

(define (material-shader m stage)
  (for/first ([s (in-list (material-shaders m))] #:when (eq? (shader-stage s) stage)) s))

;; Whether the material's shaders may write storage buffers (then no buffer block is readonly)
(define (material-writes-buffers? m)
  (for/or ([b (in-list (material-bindings m))])
    (and (memq (binding-resource-kind b) '(buffer buffer-array))
         (memq (binding-access b) '(write read-write))
         #t)))

;; ---------------------------------------------------------------------------------------------
;; Graphs

;; Expressions: literals (exact integers, reals and booleans), references to parameters, constants
;; and per-frame inputs, and operations (op is a symbol, e.g. '+ or 'vec2)
(struct e-literal (value loc) #:transparent)
(struct e-ref (name loc) #:transparent)
(struct e-op (op args loc) #:transparent)

;; A graph: its items in file order, and its nodes in file order
(struct graph (name items nodes loc) #:transparent)

;; type: a scalar-t (bool, int, uint or float)
(struct g-parameter (name type loc) #:transparent)
;; kind: 'per-frame or 'data (with a type: a value each frame, or elements C++ gives once),
;; 'meshes, 'cpu or 'extern
(struct g-input (name kind type loc) #:transparent)
;; resting: 'shader-read-only, 'general or 'transfer-src
(struct g-import (name resting loc) #:transparent)
;; Images the application owns, however many it gives, for array bindings
(struct g-import-images (name resting loc) #:transparent)
;; A buffer the application owns, holding elements of type
(struct g-import-buffer (name type loc) #:transparent)
;; A device-local buffer holding a data input's elements, uploaded when the graph is built
(struct g-upload (name source type loc) #:transparent)
;; Build-time values. type: the expression's type
(struct g-constant (name expr type loc) #:transparent)
;; options: an association list of option symbols (e.g. 'filter) to values (symbols, booleans, numbers)
(struct g-sampler (name options loc) #:transparent)
;; A per-frame buffer of type, written from the per-frame input `source`
(struct g-uniform (name type source loc) #:transparent)
;; format: a symbol (see formats in graph.rkt). extent: (list 'fixed w h), (list 'surface scale) or
;; (list 'extent-of import-name). frames: 'shared or 'per-frame. when: an expression or #f.
(struct g-image (name format extent frames when loc) #:transparent)
(struct g-buffer (name type count frames when loc) #:transparent)
;; spec: a t-attachments, t-surface or t-target-if
(struct g-target (name spec loc) #:transparent)
(struct t-attachments (colors depth loc) #:transparent)       ; depth: a t-attachment or #f
(struct t-attachment (image load store clear loc) #:transparent) ; clear: a list of 4 reals, or a real
(struct t-surface (clear depth loc) #:transparent)              ; depth: a format symbol or 'none
(struct t-target-if (condition then else loc) #:transparent)
;; An instance of a material declared in the file. bindings: (cons binding resource) in the
;; material's order, where resource is a name, or for an array binding a list of names or the name
;; of imported images. push: (cons field expression) in the material's order. options: an
;; association list of pipeline options. invocations: expressions (compute) or #f.
(struct g-material (name material bindings push options invocations loc) #:transparent)
;; kind: 'dispatch 'draw-meshes 'draw 'readback 'cpu 'copy 'clear 'extern. target, material,
;; subject (the meshes, image, cpu or extern input): names or #f. count: an expression or #f (a
;; draw's vertices, a copy's elements, a clear's value), or for a copy into an image its region's
;; (width height) expressions or #f. reads, writes: the resources a cpu node's work, a copy or a
;; clear reads and writes (names). offsets: a copy's (cons source-offset destination-offset), the
;; destination offset an image's (x y) for a copy into one; else #f. when, enabled: expressions or #f.
(struct g-node (kind name target material subject count reads writes offsets when enabled loc) #:transparent)

;; The declaration of a graph-level name
(define (graph-item-name item)
  (match item
    [(g-parameter n _ _) n] [(g-input n _ _ _) n] [(g-import n _ _) n] [(g-import-images n _ _) n] [(g-import-buffer n _ _) n] [(g-upload n _ _ _) n] [(g-constant n _ _ _) n]
    [(g-sampler n _ _) n] [(g-uniform n _ _ _) n] [(g-image n _ _ _ _ _) n] [(g-buffer n _ _ _ _ _) n]
    [(g-target n _ _) n] [(g-material n _ _ _ _ _ _) n]))
