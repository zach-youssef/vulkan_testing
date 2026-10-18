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
;; Elements apps hold (zgal::Data in C++): from readbacks, C++ and graphs' per-frame data inputs
(struct data-t (element) #:transparent)
;; An array binding: its elements' type (a texture, storage image, buffer's value or buffer-array's
;; array), chosen with `at`
(struct binding-array-t (element) #:transparent)
(struct void-t () #:transparent)
(struct values-t (types) #:transparent)

;; Declarations. Every `loc` is where the declaration is in the .zgal file.
(struct field (name type loc) #:transparent)
(struct struct-decl (name fields loc) #:transparent)
(struct vertex-format (name attributes loc) #:transparent)   ; attributes: attributes

;; A vertex-format attribute: a field (its type is what shaders read), how vertex buffers store its
;; components (#f for 32 bits, else a storage kind, below, converting to the field's kind), and the
;; value models give it when they don't (a number, a list of a vector's numbers, or #f for none)
(struct attribute field (storage default) #:transparent)
;; Storage kinds, by the kind of the attribute they're read as: normalized integers and halves read
;; as floats, small integers as uints and ints
(define attribute-storages
  (hash 'float '(float16 unorm8 snorm8 unorm16 snorm16) 'uint '(uint8 uint16) 'int '(int8 int16)))
(define all-attribute-storages '(float16 unorm8 snorm8 unorm16 snorm16 uint8 uint16 int8 int16))
(define (storage-bits s)
  (case s [(unorm8 snorm8 uint8 int8) 8] [(float16 unorm16 snorm16 uint16 int16) 16]))
;; What models (resources/Model.h) give vertex formats' attributes, chosen by the attribute's name:
;; member: ModelPrimitive's member; names: the attribute names taking it; size: its vec size; sizes:
;; the float vector sizes it converts to; fill: the components added converting it to a larger one
(struct model-attribute (member names size sizes fill) #:transparent)
(define model-attributes
  (list (model-attribute "positions" '("position" "pos") 3 '(2 3 4) 1.0)
        (model-attribute "normals" '("normal") 3 '(3 4) 0.0)
        (model-attribute "uvs" '("uv" "uv0" "texCoord" "texcoord") 2 '(2) #f)
        (model-attribute "colors" '("color" "colour") 4 '(3 4) #f)
        (model-attribute "tangents" '("tangent") 4 '(3 4) #f)))
;; The model attribute an attribute takes, or #f
(define (model-attribute-of a)
  (for/first ([m (in-list model-attributes)] #:when (member (field-name a) (model-attribute-names m))) m))

;; Whether a format has attributes stored in fewer than 32 bits
(define (vertex-format-packed? f)
  (for/or ([a (in-list (vertex-format-attributes f))]) (and (attribute-storage a) #t)))

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
;; handlers: list of handler. runs: the graphs it runs (graph-runs), the active one the first whose
;; condition holds (the last has none); empty for none.
(struct app (name parameters externs state initial handlers runs loc) #:transparent)
;; event: kind 'key-pressed 'key-released (keys: key name symbols), 'mouse-pressed 'mouse-released
;; (button: 'left 'right 'middle), 'cursor-moved 'scrolled 'frame, or 'read: a buffer readback's
;; copy has finished (readback: (list graph node element-type)). binding: the name the event's
;; value is bound to (a cursor position, scroll offset, time step or the copied data), or #f.
(struct event (kind keys button binding readback loc) #:transparent)
(struct handler (event body loc) #:transparent)
;; bindings: (cons graph-item-name expression) for the graph's parameters (computed once) and
;; per-frame inputs (each frame). enables: (cons node-name expression), computed each frame. when:
;; whether it's the active graph (an expression computed each frame), or #f for otherwise.
(struct graph-run (graph bindings enables when loc) #:transparent)


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
    [(data-t e) (format "(data ~a)" (type-name e))]
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
;; 'meshes, 'cpu, 'extern or 'path (an asset's path)
(struct g-input (name kind type loc) #:transparent)
;; resting: 'shader-read-only, 'general or 'transfer-src
(struct g-import (name resting loc) #:transparent)
;; Images the application owns, however many it gives, for array bindings
(struct g-import-images (name resting loc) #:transparent)
;; An image loaded from a file when the graph is built, sampled (resting shader-read-only).
;; source: an asset-source. color: 'srgb or 'linear.
(struct g-image-file (name source color loc) #:transparent)
;; Where an asset is: path, a string (found in the asset search paths if relative), or input, the
;; name of a path input C++ gives
(struct asset-source (path input) #:transparent)
;; A model loaded from a file when the graph is built (resources/Model.h): meshes of the vertex
;; format's vertices, which draw-meshes draws, each with its material's index as its instance index.
;; source: an asset-source. format: a vertex-format.
(struct g-model (name source format loc) #:transparent)
;; A model's materials' base colour textures (images, for array bindings) and factors (a buffer of
;; vec4s), one per material. model: the model's name.
(struct g-model-textures (name model loc) #:transparent)
(struct g-model-factors (name model loc) #:transparent)
;; A model's draws (a buffer of DrawIndexedCommands, type: their struct-t, from (import indirect)),
;; one per primitive of its mesh, and their bounding spheres (a buffer of vec4s: centre, radius)
(struct g-model-draws (name model type loc) #:transparent)
(struct g-model-bounds (name model loc) #:transparent)
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
;; once?: whether it runs in the graph's first frame only (RenderGraph::runOnce). extra: what a kind
;; needs beyond these, else #f: a kernel's steps (k-steps).
(struct g-node (kind name target material subject count reads writes offsets when enabled once? extra loc) #:transparent)
;; A step of a kernel node (a library algorithm, e.g. a scan): a dispatch of a material instance
;; (its name), with its push constant `pass` replaced by pass (an integer) if pass isn't #f
(struct k-step (material pass) #:transparent)
;; An indirect dispatch's or draw's commands: command, the first (an element-ref of a graph buffer
;; of commands); draws, how many a draw draws (at most, with a count; a build-time expression), or
;; #f for a dispatch; count, an element-ref of how many to draw, or #f. limit: for a count on devices
;; without drawIndirectCount, the material instance copying the commands with those past the count
;; given no instances, and the buffer it copies them into (names), or #f.
(struct indirect (command draws count limit limited) #:transparent)
;; A buffer element, e.g. a count shaders wrote: the buffer's name and the element's index (a
;; build-time expression)
(struct element-ref (buffer index loc) #:transparent)

;; The declaration of a graph-level name
(define (graph-item-name item)
  (match item
    [(g-parameter n _ _) n] [(g-input n _ _ _) n] [(g-import n _ _) n] [(g-image-file n _ _ _) n] [(g-model n _ _ _) n] [(g-model-textures n _ _) n] [(g-model-factors n _ _) n] [(g-model-draws n _ _ _) n] [(g-model-bounds n _ _) n] [(g-import-images n _ _) n] [(g-import-buffer n _ _) n] [(g-upload n _ _ _) n] [(g-constant n _ _ _) n]
    [(g-sampler n _ _) n] [(g-uniform n _ _ _) n] [(g-image n _ _ _ _ _) n] [(g-buffer n _ _ _ _ _) n]
    [(g-target n _ _) n] [(g-material n _ _ _ _ _ _) n]))
