#lang racket/base
;; The checked program: what the parser produces and the backends consume. It describes the
;; interface (types, bindings, stages) without assuming a backend, so adding a backend only adds an
;; emitter.

(require racket/match)

(provide (all-defined-out))

;; Types
(struct scalar-t (kind) #:transparent)          ; 'float 'int 'uint 'bool
(struct vector-t (kind size) #:transparent)     ; kind of the components, 2-4 of them
(struct matrix-t (size) #:transparent)          ; square float matrices, 2-4 columns
(struct array-t (element length) #:transparent) ; fixed length
(struct struct-t (decl) #:transparent)          ; a struct-decl

;; Declarations. Every `loc` is where the declaration is in the .zgal file.
(struct field (name type loc) #:transparent)
(struct struct-decl (name fields loc) #:transparent)
(struct vertex-format (name attributes loc) #:transparent)   ; attributes: fields

;; path: the shader body's path (relative to the working directory if the .zgal path was).
;; name: its file name, which the shader library finds it by. stage: 'vertex 'fragment 'compute.
(struct shader (path name stage loc) #:transparent)

;; kind: 'texture 'sampler 'storage-image 'buffer 'buffer-array
;; element: the type a buffer holds (or holds an array of), else #f
;; access: 'read 'write 'read-write, or #f for textures and samplers
;; stages: the stages using it (a subset of the material's), or '() for all of them
(struct binding (name kind element access stages loc) #:transparent)

;; attributes: the format's attributes read, as (cons index field); the index is the location
(struct vertex-input (format attributes loc) #:transparent)

;; interpolation: 'smooth 'flat 'noperspective
(struct varying (name type interpolation loc) #:transparent)

;; kind: 'graphics or 'compute. shaders: list of shader. push: fields. vertex-input: or #f.
;; varyings, outputs: lists (graphics only). workgroup: (list x y z) for compute, else #f.
(struct material (kind name shaders bindings push vertex-input varyings outputs workgroup loc)
  #:transparent)

;; source: the .zgal file's path. Declarations are in file order.
(struct program (source structs vertex-formats materials graphs) #:transparent)

(define (type-name t)
  (match t
    [(scalar-t k) (symbol->string k)]
    [(vector-t k n) (format "~a~a" (case k [(float) "vec"] [(int) "ivec"] [(uint) "uvec"] [(bool) "bvec"]) n)]
    [(matrix-t n) (format "mat~a" n)]
    [(array-t e n) (format "(array ~a ~a)" (type-name e) n)]
    [(struct-t d) (struct-decl-name d)]))

(define (material-shader m stage)
  (for/first ([s (in-list (material-shaders m))] #:when (eq? (shader-stage s) stage)) s))

;; Whether the material's shaders may write storage buffers (then no buffer block is readonly)
(define (material-writes-buffers? m)
  (for/or ([b (in-list (material-bindings m))])
    (and (memq (binding-kind b) '(buffer buffer-array))
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
;; kind: 'per-frame (with a type), 'meshes or 'cpu
(struct g-input (name kind type loc) #:transparent)
;; resting: 'shader-read-only, 'general or 'transfer-src
(struct g-import (name resting loc) #:transparent)
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
;; spec: a t-attachments, t-surface or t-if
(struct g-target (name spec loc) #:transparent)
(struct t-attachments (colors depth loc) #:transparent)       ; depth: a t-attachment or #f
(struct t-attachment (image load store clear loc) #:transparent) ; clear: a list of 4 reals, or a real
(struct t-surface (clear depth loc) #:transparent)              ; depth: a format symbol or 'none
(struct t-if (condition then else loc) #:transparent)
;; An instance of a material declared in the file. bindings: (cons binding resource-name) in the
;; material's order. push: (cons field expression) in the material's order. options: an
;; association list of pipeline options. invocations: expressions (compute) or #f.
(struct g-material (name material bindings push options invocations loc) #:transparent)
;; kind: 'dispatch 'draw-meshes 'draw 'readback 'cpu. target, material, subject (the meshes,
;; image or cpu input): names or #f. count: an expression or #f. when, enabled: expressions or #f.
(struct g-node (kind name target material subject count when enabled loc) #:transparent)

;; The declaration of a graph-level name
(define (graph-item-name item)
  (match item
    [(g-parameter n _ _) n] [(g-input n _ _ _) n] [(g-import n _ _) n] [(g-constant n _ _ _) n]
    [(g-sampler n _ _) n] [(g-uniform n _ _ _) n] [(g-image n _ _ _ _ _) n] [(g-buffer n _ _ _ _ _) n]
    [(g-target n _ _) n] [(g-material n _ _ _ _ _ _) n]))
