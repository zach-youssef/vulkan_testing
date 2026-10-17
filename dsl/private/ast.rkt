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
(struct program (source structs vertex-formats materials) #:transparent)

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
