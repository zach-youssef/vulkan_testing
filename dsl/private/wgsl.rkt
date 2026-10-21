#lang racket/base
;; The WGSL backend (WebGPU): one module per material shader. WebGPU has no bindless descriptors, so
;; a material's bindings are a bind group of their own (group 0, binding i for the material's i'th
;; binding) and its push constants a uniform buffer after them (the TypeScript runtime gives each
;; node's push constants their own offset in it).
;;
;; WGSL has no 8- or 16-bit integers, and atomics only on atomic<u32> and atomic<i32> variables. A
;; buffer or shared variable holding small integers, or one that atomics act on, is declared as
;; 32-bit words ("raw") and read and written through generated functions at its std430 byte offsets,
;; so its memory is laid out exactly as the C++ and the GLSL backend lay it out. Others are declared
;; with their own types. A raw variable is atomic<u32> words when atomics act on it or a write
;; changes part of a word (which other invocations may be writing the rest of).

(require racket/list
         racket/match
         racket/set
         racket/string
         "ast.rkt"
         "body.rkt"
         "builtins.rkt"
         "errors.rkt"
         "layout.rkt"
         "lower.rkt"
         (only-in "parse.rkt" input-variable-name))

(provide emit-wgsl
         wgsl-shader-file-name
         (struct-out wgsl-binding)
         wgsl-bindings
         wgsl-push-layout
         wgsl-push-binding
         wgsl-name
         wgsl-reserved-words)

;; The file a shader's module is written to: its name plus .wgsl, e.g. Lit.frag.wgsl
(define (wgsl-shader-file-name s) (string-append (shader-name s) ".wgsl"))

;; ---------------------------------------------------------------------------------------------
;; Names

;; WGSL's keywords, reserved words and the predeclared names generated code uses. Program names
;; that are one are spelled with a zgal_ prefix, which programs can't declare.
(define wgsl-reserved-words
  (list->set
   (append
    '("alias" "break" "case" "const" "const_assert" "continue" "continuing" "default" "diagnostic"
      "discard" "else" "enable" "false" "fn" "for" "if" "let" "loop" "override" "requires" "return"
      "struct" "switch" "true" "var" "while"
      "NULL" "Self" "abstract" "active" "alignas" "alignof" "as" "asm" "asm_fragment" "async"
      "attribute" "auto" "await" "become" "cast" "catch" "class" "co_await" "co_return" "co_yield"
      "coherent" "column_major" "common" "compile" "compile_fragment" "concept" "const_cast"
      "consteval" "constexpr" "constinit" "crate" "debugger" "decltype" "delete" "demote"
      "demote_to_helper" "do" "dynamic_cast" "enum" "explicit" "export" "extends" "extern" "external"
      "fallthrough" "filter" "final" "finally" "friend" "from" "fxgroup" "get" "goto" "groupshared"
      "highp" "impl" "implements" "import" "inline" "instanceof" "interface" "layout" "lowp" "macro"
      "macro_rules" "match" "mediump" "meta" "mod" "module" "move" "mut" "mutable" "namespace" "new"
      "nil" "noexcept" "noinline" "nointerpolation" "non_coherent" "noncoherent" "noperspective"
      "null" "nullptr" "of" "operator" "package" "packoffset" "partition" "pass" "patch"
      "pixelfragment" "precise" "precision" "premerge" "priv" "protected" "pub" "public" "readonly"
      "ref" "regardless" "register" "reinterpret_cast" "require" "resource" "restrict" "self" "set"
      "shared" "signed" "sizeof" "smooth" "snorm" "static" "static_assert" "static_cast" "std"
      "subroutine" "super" "target" "template" "this" "thread_local" "throw" "trait" "try" "type"
      "typedef" "typeid" "typename" "typeof" "union" "unless" "unorm" "unsafe" "unsized" "use"
      "using" "varying" "virtual" "volatile" "wgsl" "where" "with" "writeonly" "yield"
      ;; Predeclared types and the access modes and address spaces
      "bool" "f16" "f32" "i32" "u32" "array" "atomic" "ptr" "sampler" "sampler_comparison"
      "texture_2d" "texture_depth_2d" "texture_storage_2d" "read" "write" "read_write" "function"
      "private" "storage" "uniform" "workgroup"
      ;; Predeclared functions generated code calls that aren't GLSL's
      "select" "bitcast" "atan2" "inverseSqrt" "faceForward" "dpdx" "dpdy" "dpdxFine" "dpdyFine"
      "dpdxCoarse" "dpdyCoarse" "countOneBits" "saturate" "workgroupBarrier" "storageBarrier"
      "textureSample" "textureSampleLevel" "textureSampleCompare" "textureSampleCompareLevel"
      "textureLoad" "textureStore" "textureDimensions" "atomicLoad" "atomicStore" "atomicAdd"
      "atomicMin" "atomicMax" "atomicAnd" "atomicOr" "atomicXor" "atomicExchange"
      "atomicCompareExchangeWeak" "subgroupShuffleUp" "subgroupShuffleDown" "arrayLength")
    (for*/list ([n '(2 3 4)] [suffix '("" "f" "i" "u" "h")]) (format "vec~a~a" n suffix))
    (for*/list ([c '(2 3 4)] [r '(2 3 4)] [suffix '("" "f" "h")]) (format "mat~ax~a~a" c r suffix)))))

(define (wgsl-name n)
  (if (set-member? wgsl-reserved-words n) (string-append "zgal_" n) n))

;; An error for what WGSL (or WebGPU) can't express
(define (web-error where fmt . args)
  (apply zgal-error 'not-on-web where fmt args))

;; ---------------------------------------------------------------------------------------------
;; Types

(define (scalar-name k)
  (case k
    [(float) "f32"] [(int int8 int16) "i32"] [(uint uint8 uint16) "u32"] [(bool) "bool"]))

;; A value's type (small integers are 32 bits in values: they're storage types)
(define (wgsl-type t [where #f])
  (match t
    [(scalar-t k) (scalar-name k)]
    [(vector-t 'bool n) (format "vec~a<bool>" n)]
    [(vector-t k n) (format "vec~a~a" n (case k [(float) "f"] [(int) "i"] [(uint) "u"]))]
    [(matrix-t n) (format "mat~ax~af" n n)]
    [(array-t e n) (format "array<~a, ~a>" (wgsl-type e where) n)]
    [(runtime-array-t e) (format "array<~a>" (wgsl-type e where))]
    [(struct-t d) (wgsl-name (struct-decl-name d))]
    [(void-t) #f]
    [_ (web-error where "~a has no WGSL type" (type-name t))]))

(define (integer-kind? k) (and (memq k '(int uint int8 uint8 int16 uint16)) #t))
(define (signed-kind? k) (and (memq k '(int int8 int16)) #t))

;; ---------------------------------------------------------------------------------------------
;; Bindings and push constants, as the runtime lays out the bind group

;; A binding as WebGPU binds it. index: its @binding. kind: 'texture 'texture-array 'sampler
;; 'storage-image 'buffer 'buffer-array ('texture-array: an array binding of textures, which WebGPU
;; has no arrays of bindings for, as a texture_2d_array whose layers are its elements). access: 'read 'write 'read-write (buffers, storage images) or #f.
;; stages: the stages using it ('vertex 'fragment 'compute). depth?: a texture sampled with a
;; comparison (texture_depth_2d) or a comparison sampler.
(struct wgsl-binding (name index kind access stages depth?) #:transparent)

;; m's bindings in bind group order, with which stages use them and whether they compare depths
(define (wgsl-bindings prog m)
  (define uses (for/hash ([s (in-list (material-shaders m))]) (values (shader-stage s) (stage-usage prog m s))))
  (for/list ([b (in-list (material-bindings m))] [i (in-naturals)])
    (when (and (eq? (binding-kind b) 'array) (not (eq? (binding-resource-kind b) 'texture)))
      (web-error (binding-loc b) "WebGPU has no arrays of bindings (descriptor indexing), so ~a can't be an array of ~as (arrays of textures are texture arrays on the web)"
                 (binding-name b) (binding-resource-kind b)))
    (define stages
      (for/list ([s (in-list (material-shaders m))]
                 #:when (set-member? (usage-bindings (hash-ref uses (shader-stage s))) (binding-name b)))
        (shader-stage s)))
    (wgsl-binding (binding-name b) i (if (eq? (binding-kind b) 'array) 'texture-array (binding-kind b)) (binding-access b) stages
                  (for/or ([u (in-hash-values uses)]) (set-member? (usage-depth u) (binding-name b))))))

;; The binding index of m's push constants' uniform buffer
(define (wgsl-push-binding m) (length (material-bindings m)))

;; The uniform layout of m's push constants: (list (cons field offset) ...) and the size (a
;; multiple of 16), or #f without push constants. Uniform buffers align vectors as std430 does,
;; but arrays and structs to 16 bytes, so push constants are scalars, vectors and matrices.
(define (wgsl-push-layout m)
  (define push (material-push m))
  (cond
    [(null? push) #f]
    [else
     (for ([f (in-list push)])
       (unless (or (scalar-t? (field-type f)) (vector-t? (field-type f)) (matrix-t? (field-type f)))
         (web-error (field-loc f) "push constant ~a is ~a, but WebGPU's push constants (a uniform buffer) are scalars, vectors and matrices"
                    (field-name f) (type-name (field-type f)))))
     (define-values (placed end) (place-fields push 0))
     (cons (for/list ([p (in-list placed)]) (cons (placed-field p) (placed-offset p)))
           (round-up (max end 4) 16))]))

;; ---------------------------------------------------------------------------------------------
;; What a shader's body does with the material's variables

;; bindings: the names of the bindings it uses. depth: the textures and samplers it compares
;; depths with. raw: the buffers and shared variables declared as words (see the top), and
;; atomic: those of them that are atomic words.
(struct usage (bindings depth raw atomic) #:transparent)

(define (body-of s)
  (unless (zgal-shader? s)
    (web-error (shader-loc s) "~a is a GLSL body, which WebGPU can't use: write the shader in ZGAL" (shader-path s)))
  (zgal-shader-body s))

;; The root variable of a place: the t-global it indexes or takes fields of, or #f
(define (place-root e)
  (match e
    [(t-global (or 'binding 'shared) _ _ _ _) e]
    [(or (t-field b _ _ _) (t-index b _ _ _) (t-swizzle b _ _ _)) (place-root b)]
    [_ #f]))

;; The type a variable's memory holds: a buffer's value, a buffer-array's array, a shared variable's
(define (variable-type g) (t-global-type g))

;; Whether memory of type t holds small integers
(define (holds-small? t) (pair? (small-integer-kinds-in t)))

(define (stage-usage prog m s)
  (define body (body-of s))
  (define all (subexpressions body))
  (define bindings (for/set ([e (in-list all)] #:when (and (t-global? e) (eq? (t-global-kind e) 'binding))) (t-global-name e)))
  (define depth (depth-names body (program-function-table prog)))
  (define atomic-targets
    (for/set ([e (in-list all)] #:when (t-atomic? e)) (t-global-name (place-root (t-atomic-place e)))))
  (define raw
    (set-union atomic-targets
               (for/set ([e (in-list all)]
                         #:when (and (t-global? e) (memq (t-global-kind e) '(binding shared))
                                     (holds-small? (variable-type e))))
                 (t-global-name e))))
  ;; Writes changing part of a word
  (define partial-writes
    (for/set ([e (in-list all)]
              #:when (and (t-write? e) (place-root (t-write-place e))
                          (set-member? raw (t-global-name (place-root (t-write-place e))))
                          (needs-read-modify-write? (texpr-type (t-write-place e)) (address-alignment (t-write-place e)))))
      (t-global-name (place-root (t-write-place e)))))
  (usage bindings depth raw (set-union atomic-targets partial-writes)))

;; The program's functions and constants by name
(define (program-function-table prog)
  (for/hash ([d (in-list (program-functions prog))])
    (values (if (function-decl? d) (function-decl-name d) (constant-decl-name d)) d)))

;; The names of the textures and samplers (bindings, or a function's parameters) body compares
;; depths with, directly or through the functions it calls
(define (depth-names body functions)
  (for/fold ([names (set)]) ([e (in-list (subexpressions body))])
    (match e
      [(t-texture 'sample-compare (list t s _ _) _ _) (set-add (set-add names (opaque-name t)) (opaque-name s))]
      [(t-user-call f args _ _)
       (define d (hash-ref functions f #f))
       (define inner (if (and d (function-decl-body d)) (depth-names (function-decl-body d) functions) (set)))
       (for/fold ([names names]) ([p (in-list (if d (function-decl-params d) '()))] [a (in-list args)])
         (if (and (set-member? inner (car p)) (opaque-name a)) (set-add names (opaque-name a)) names))]
      [_ names])))

(define (opaque-name e)
  (match e
    [(t-global _ name _ _ _) name]
    [(t-local name _ _) name]
    [_ #f]))

;; ---------------------------------------------------------------------------------------------
;; Addresses in raw variables: a constant byte offset plus indexes times strides

(struct address (root constant terms) #:transparent)   ; terms: (cons index-expression stride)

(define (address-of place)
  (match place
    [(t-global _ _ _ _ _) (address place 0 '())]
    [(t-field b name _ _)
     (match-define (address root c ts) (address-of b))
     (define d (struct-t-decl (texpr-type b)))
     (define-values (placed _end) (place-fields (struct-decl-fields d) 0))
     (define offset (for/first ([p (in-list placed)] #:when (string=? (field-name (placed-field p)) name)) (placed-offset p)))
     (address root (+ c offset) ts)]
    [(t-index b i _ _)
     (match-define (address root c ts) (address-of b))
     (define stride
       (match (texpr-type b)
         [(array-t e _) (std430-stride e)]
         [(runtime-array-t e) (std430-stride e)]
         [(vector-t _ _) 4]
         [(matrix-t n) (std430-stride (vector-t 'float n))]))
     (match i
       [(t-lit v _ _) (address root (+ c (* v stride)) ts)]
       [_ (address root c (append ts (list (cons i stride))))])]
    [(t-swizzle b comps _ _)
     (match-define (address root c ts) (address-of b))
     (address root (+ c (* 4 (string-index "xyzw" (string-ref comps 0)))) ts)]))

(define (string-index s ch)
  (for/first ([c (in-string s)] [i (in-naturals)] #:when (char=? c ch)) i))

;; The largest of 4, 2 and 1 known to divide a place's offset
(define (address-alignment place)
  (match-define (address _ c ts) (address-of place))
  (for/first ([a (in-list '(4 2 1))]
              #:when (and (zero? (remainder c a)) (for/and ([t (in-list ts)]) (zero? (remainder (cdr t) a)))))
    a))

;; The scalars making up memory of type t: (list offset kind path), where path gives the scalar from
;; a value of type t (field names and indexes)
(define (leaves t)
  (match t
    [(scalar-t k) (list (list 0 k '()))]
    [(vector-t k n) (for/list ([i (in-range n)]) (list (* 4 i) k (list i)))]
    [(matrix-t n)
     (for*/list ([c (in-range n)] [r (in-range n)])
       (list (+ (* c (std430-stride (vector-t 'float n))) (* 4 r)) 'float (list c r)))]
    [(array-t e n)
     (for*/list ([i (in-range n)] [l (in-list (leaves e))])
       (list (+ (* i (std430-stride e)) (first l)) (second l) (cons i (third l))))]
    [(struct-t d)
     (define-values (placed _end) (place-fields (struct-decl-fields d) 0))
     (for*/list ([p (in-list placed)] [l (in-list (leaves (field-type (placed-field p))))])
       (list (+ (placed-offset p) (first l)) (second l) (cons (field-name (placed-field p)) (third l))))]))

(define (kind-bytes k) (if (small-integer-kind? k) (quotient (small-integer-bits k) 8) 4))

;; Whether a value of type t at an offset aligned to `alignment` fills whole words, so it can be
;; written a word at a time
(define (fills-words? t alignment)
  (and (= alignment 4)
       (zero? (remainder (std430-size t) 4))
       (<= (length (leaves t)) 256)
       (= (for/sum ([l (in-list (leaves t))]) (kind-bytes (second l))) (std430-size t))))

(define (needs-read-modify-write? t alignment)
  (and (holds-small? t) (not (fills-words? t alignment))))

;; ---------------------------------------------------------------------------------------------
;; Emitting a module

;; The module of shader s of material m. storage-format: the texel format of a storage image
;; binding (a binding), e.g. "rgba8unorm": WGSL declares it in the shader.
(define (emit-wgsl prog m s #:storage-format [storage-format (λ (b) "rgba8unorm")])
  (define body (body-of s))
  (define stage (shader-stage s))
  (define u (stage-usage prog m s))
  (define layout (wgsl-bindings prog m))
  (define push-layout (wgsl-push-layout m))
  (define functions (program-function-table prog))
  (for ([name (in-list (zgal-shader-uses s))])
    (define d (hash-ref functions name))
    (when (glsl-function-decl? d)
      (web-error (function-decl-loc d) "~a is written in GLSL (extern-glsl), which WebGPU can't use: write it in ZGAL" name)))
  (define st (make-state stage u m functions))

  ;; The functions and body first, so the helpers and built-ins they need are known
  (define function-lines
    (append*
     (for/list ([name (in-list (zgal-shader-uses s))])
       (define d (hash-ref functions name))
       (if (function-decl? d) (emit-function st d) (emit-constant st d)))))
  (define main-lines (emit-main st s body))

  (define out '())
  (define (line! fmt . args) (set! out (cons (apply format fmt args) out)))
  (line! "// Generated by zgal from ~a: material ~a's ~a shader, for WebGPU." (program-source prog) (material-name m) stage)
  (line! "// Don't edit it: edit that file.")
  (when (state-subgroups? st) (line! "enable subgroups;"))
  (line! "diagnostic(off, derivative_uniformity);")
  (when (state-subgroups? st) (line! "diagnostic(off, subgroup_uniformity);"))

  ;; Every struct in the file, as values (small integers widened)
  (for ([d (in-list (program-structs prog))])
    (line! "")
    (line! "struct ~a {" (wgsl-name (struct-decl-name d)))
    (for ([f (in-list (struct-decl-fields d))])
      (line! "    ~a: ~a," (wgsl-name (field-name f)) (wgsl-type (field-type f) (field-loc f))))
    (line! "}"))

  ;; Bindings this stage uses, then the push constants
  (line! "")
  (for ([b (in-list (material-bindings m))] [wb (in-list layout)]
        #:when (set-member? (usage-bindings u) (binding-name b)))
    (line! "@group(0) @binding(~a) var~a ~a: ~a;" (wgsl-binding-index wb)
           (if (memq (binding-kind b) '(buffer buffer-array))
               (format "<storage, ~a>" (if (eq? (binding-access b) 'read) "read" "read_write"))
               "")
           (wgsl-name (binding-name b))
           (match (binding-kind b)
             ['texture (if (wgsl-binding-depth? wb) "texture_depth_2d" "texture_2d<f32>")]
             ['array "texture_2d_array<f32>"]
             ['sampler (if (wgsl-binding-depth? wb) "sampler_comparison" "sampler")]
             ['storage-image (format "texture_storage_2d<~a, ~a>" (storage-format b)
                                     (case (binding-access b) [(read) "read"] [(write) "write"] [else "read_write"]))]
             [(or 'buffer 'buffer-array)
              (if (set-member? (usage-raw u) (binding-name b))
                  (raw-declaration-type st (binding-name b) #f)
                  (wgsl-type (if (eq? (binding-kind b) 'buffer) (binding-element b) (runtime-array-t (binding-element b)))
                             (binding-loc b)))])))
  (when (and push-layout (state-push-used? st))
    (line! "")
    (line! "struct ZgalPush {")
    (for ([entry (in-list (car push-layout))])
      (line! "    ~a: ~a," (wgsl-name (field-name (car entry))) (wgsl-type (field-type (car entry)))))
    (line! "}")
    (line! "@group(0) @binding(~a) var<uniform> zgalPush: ZgalPush;" (wgsl-push-binding m)))
  (for ([f (in-list (material-shared m))])
    (line! "var<workgroup> ~a: ~a;" (wgsl-name (field-name f))
           (if (set-member? (usage-raw u) (field-name f))
               (raw-declaration-type st (field-name f) (field-type f))
               (wgsl-type (field-type f) (field-loc f)))))
  (for ([v (in-list (state-privates st))])
    (line! "var<private> ~a;" v))

  (for ([h (in-list (reverse (state-helper-texts st)))])
    (line! "")
    (for ([l (in-list (string-split h "\n"))]) (line! "~a" l)))
  (for ([l (in-list function-lines)]) (line! "~a" l))
  (for ([l (in-list main-lines)]) (line! "~a" l))
  (string-append (string-join (reverse out) "\n") "\n"))

;; ---------------------------------------------------------------------------------------------
;; The emitter's state for one module: the stage, what the body does, the helper functions emitted
;; (by name, with their texts in order), private variables, and whether it uses subgroups and push
;; constants

(struct state (stage usage material functions helpers [helper-texts #:mutable] [privates #:mutable]
                     [subgroups? #:mutable] [push-used? #:mutable] [entry-builtins #:mutable]
                     [depth #:mutable] [function-constants #:mutable]))

(define (make-state stage u m functions) (state stage u m functions (make-hash) '() '() #f #f '() (usage-depth u) (set)))

;; Adds helper `name` once, its text made by make-text (called only the first time, so it can
;; add the helpers it calls first)
(define (helper! st name make-text)
  (unless (hash-ref (state-helpers st) name #f)
    (hash-set! (state-helpers st) name #t)
    (define text (make-text))
    (set-state-helper-texts! st (cons text (state-helper-texts st))))
  name)

(define (private! st declaration)
  (unless (member declaration (state-privates st))
    (set-state-privates! st (append (state-privates st) (list declaration)))))

;; A built-in of the entry point (its WGSL name and type), as the parameter name it's given
(define (entry-builtin! st builtin type [name (string-append "zgal" (string-titlecase-camel builtin))])
  (unless (assoc builtin (state-entry-builtins st))
    (set-state-entry-builtins! st (append (state-entry-builtins st) (list (list builtin name type)))))
  name)

;; global_invocation_id -> GlobalInvocationId
(define (string-titlecase-camel s)
  (string-append* (for/list ([part (in-list (string-split s "_"))])
                    (string-append (string-upcase (substring part 0 1)) (substring part 1)))))

;; ---------------------------------------------------------------------------------------------
;; Raw variables: their declarations and the functions reading and writing them

(define (raw-atomic? st name) (set-member? (usage-atomic (state-usage st)) name))

;; The declared type of raw variable `name`: words, a runtime array for buffers (t #f), else
;; enough of them for t
(define (raw-declaration-type st name t)
  (define word (if (raw-atomic? st name) "atomic<u32>" "u32"))
  (if t
      (format "array<~a, ~a>" word (max 1 (quotient (+ (std430-size t) 3) 4)))
      (format "array<~a>" word)))

;; A key naming type t in helper names
(define (type-key t)
  (match t
    [(scalar-t k) (symbol->string k)]
    [(vector-t k n) (format "~a~a" (symbol->string k) n)]
    [(matrix-t n) (format "mat~a" n)]
    [(array-t e n) (format "~a~a" (type-key e) n)]
    [(struct-t d) (struct-decl-name d)]))

(define (word-read st var index)
  (if (raw-atomic? st var)
      (format "atomicLoad(&~a[~a])" (wgsl-name var) index)
      (format "~a[~a]" (wgsl-name var) index)))

;; The function reading a value of type t from raw variable var at a byte offset
(define (load-helper st var t)
  (define name (format "zgalLoad_~a_~a" var (type-key t)))
  (helper! st name
    (λ ()
      (define (word index) (word-read st var index))
      (define body
        (match t
          [(scalar-t 'uint) (format "return ~a;" (word "o >> 2u"))]
          [(scalar-t 'int) (format "return bitcast<i32>(~a);" (word "o >> 2u"))]
          [(scalar-t 'float) (format "return bitcast<f32>(~a);" (word "o >> 2u"))]
          [(scalar-t 'uint8) (format "return (~a >> ((o & 3u) * 8u)) & 0xffu;" (word "o >> 2u"))]
          [(scalar-t 'uint16) (format "return (~a >> ((o & 2u) * 8u)) & 0xffffu;" (word "o >> 2u"))]
          [(scalar-t 'int8) (format "return bitcast<i32>(~a << ((3u - (o & 3u)) * 8u)) >> 24u;" (word "o >> 2u"))]
          [(scalar-t 'int16) (format "return bitcast<i32>(~a << ((2u - (o & 2u)) * 8u)) >> 16u;" (word "o >> 2u"))]
          [(vector-t k n)
           (define c (load-helper st var (scalar-t k)))
           (format "return ~a(~a);" (wgsl-type t) (string-join (for/list ([i (in-range n)]) (format "~a(o + ~au)" c (* 4 i))) ", "))]
          [(matrix-t n)
           (define c (load-helper st var (vector-t 'float n)))
           (define stride (std430-stride (vector-t 'float n)))
           (format "return ~a(~a);" (wgsl-type t) (string-join (for/list ([i (in-range n)]) (format "~a(o + ~au)" c (* stride i))) ", "))]
          [(array-t e n)
           (define el (load-helper st var e))
           (format "var a: ~a;\n    for (var i = 0u; i < ~au; i += 1u) {\n        a[i] = ~a(o + i * ~au);\n    }\n    return a;"
                   (wgsl-type t) n el (std430-stride e))]
          [(struct-t d)
           (define-values (placed _end) (place-fields (struct-decl-fields d) 0))
           (format "return ~a(~a);" (wgsl-type t)
                   (string-join (for/list ([p (in-list placed)])
                                  (format "~a(o + ~au)" (load-helper st var (field-type (placed-field p))) (placed-offset p)))
                                ", "))]))
      (format "fn ~a(o: u32) -> ~a {\n    ~a\n}" name (wgsl-type t) body))))

(define (word-write st var index value)
  (if (raw-atomic? st var)
      (format "atomicStore(&~a[~a], ~a);" (wgsl-name var) index value)
      (format "~a[~a] = ~a;" (wgsl-name var) index value)))

;; A scalar of kind k as the bits of a word (its low bytes, for small integers)
(define (scalar-bits k value)
  (case k
    [(uint uint8 uint16) value]
    [else (format "bitcast<u32>(~a)" value)]))

;; The function writing a value of type t to raw variable var at a byte offset: a word at a time if
;; aligned (a value filling whole words), else a scalar at a time, changing small integers' bytes
;; in their words atomically
(define (store-helper st var t aligned?)
  (define words? (and aligned? (holds-small? t) (fills-words? t 4)))
  (define name (format "zgalStore~a_~a_~a" (if words? "Words" "") var (type-key t)))
  (helper! st name
    (λ ()
      (define body
        (cond
          [words?
           (define by-word (make-hash))
           (for ([l (in-list (leaves t))])
             (hash-update! by-word (quotient (first l) 4) (λ (xs) (cons l xs)) '()))
           (string-join
            (for/list ([w (in-list (sort (hash-keys by-word) <))])
              (word-write st var (format "(o >> 2u) + ~au" w)
                          (string-join
                           (for/list ([l (in-list (reverse (hash-ref by-word w)))])
                             (match-define (list offset k path) l)
                             (define bits (scalar-bits k (string-append "v" (path-text path))))
                             (define shift (* 8 (remainder offset 4)))
                             (define masked (if (small-integer-kind? k) (format "(~a & 0x~au)" bits (if (= (kind-bytes k) 1) "ff" "ffff")) bits))
                             (if (zero? shift) masked (format "(~a << ~au)" masked shift)))
                           " | ")))
            "\n    ")]
          [else
           (match t
             [(scalar-t k) #:when (small-integer-kind? k)
              (unless (raw-atomic? st var) (error 'wgsl "internal error: a partial write to ~a, which isn't atomic" var))
              (define mask (if (= (kind-bytes k) 1) "0xffu" "0xffffu"))
              (define shift (if (= (kind-bytes k) 1) "((o & 3u) * 8u)" "((o & 2u) * 8u)"))
              (format "let shift = ~a;\n    atomicAnd(&~a[o >> 2u], ~~(~a << shift));\n    atomicOr(&~a[o >> 2u], (~a & ~a) << shift);"
                      shift (wgsl-name var) mask (wgsl-name var) (scalar-bits k "v") mask)]
             [(scalar-t k) (word-write st var "o >> 2u" (scalar-bits k "v"))]
             [(vector-t k n)
              (define c (store-helper st var (scalar-t k) #f))
              (string-join (for/list ([i (in-range n)]) (format "~a(o + ~au, v[~a]);" c (* 4 i) i)) "\n    ")]
             [(matrix-t n)
              (define c (store-helper st var (vector-t 'float n) #f))
              (define stride (std430-stride (vector-t 'float n)))
              (string-join (for/list ([i (in-range n)]) (format "~a(o + ~au, v[~a]);" c (* stride i) i)) "\n    ")]
             [(array-t e n)
              (format "for (var i = 0u; i < ~au; i += 1u) {\n        ~a(o + i * ~au, v[i]);\n    }"
                      n (store-helper st var e #f) (std430-stride e))]
             [(struct-t d)
              (define-values (placed _end) (place-fields (struct-decl-fields d) 0))
              (string-join (for/list ([p (in-list placed)])
                             (format "~a(o + ~au, v.~a);" (store-helper st var (field-type (placed-field p)) #f)
                                     (placed-offset p) (wgsl-name (field-name (placed-field p)))))
                           "\n    ")])]))
      (format "fn ~a(o: u32, v: ~a) {\n    ~a\n}" name (wgsl-type t) body))))

(define (path-text path)
  (string-append* (for/list ([p (in-list path)]) (if (string? p) (string-append "." (wgsl-name p)) (format "[~a]" p)))))

;; A raw place's byte offset
(define (offset-text st a)
  (match-define (address _ c ts) a)
  (define terms (for/list ([t (in-list ts)]) (format "~a * ~au" (as-u32 st (car t)) (cdr t))))
  (cond
    [(null? terms) (format "~au" c)]
    [(zero? c) (string-join terms " + ")]
    [else (string-join (cons (format "~au" c) terms) " + ")]))

;; An int or uint expression as a u32
(define (as-u32 st e)
  (define text (print-expr st e))
  (if (equal? (texpr-type e) (scalar-t 'uint)) (wrap e text) (format "u32(~a)" text)))

(define (raw-place? st e)
  (define root (place-root e))
  (and root (set-member? (usage-raw (state-usage st)) (t-global-name root))))

;; ---------------------------------------------------------------------------------------------
;; Expressions

(define (atomic-expr? e)
  (match e
    [(or (t-lit _ _ _) (t-local _ _ _) (t-global _ _ _ _ _) (t-call _ _ _ _) (t-user-call _ _ _ _)
         (t-texture _ _ _ _) (t-construct _ _ _) (t-field _ _ _ _) (t-index _ _ _ _) (t-swizzle _ _ _ _)) #t]
    [_ #f]))

(define (wrap e text) (if (atomic-expr? e) text (format "(~a)" text)))

(define (literal-text v t)
  (match t
    [(scalar-t 'bool) (if v "true" "false")]
    [(scalar-t 'float)
     (define text (number->string (exact->inexact v)))
     (string-append (if (regexp-match? #rx"[.e]" text) text (string-append text ".0")) "f")]
    [(scalar-t (or 'uint 'uint8 'uint16)) (format "~au" v)]
    [(scalar-t (or 'int 'int8 'int16))
     (if (= v (- (expt 2 31))) "(-2147483647i - 1i)" (format "~ai" v))]))

;; The WGSL built-in a GLSL-named one is, where only the name differs
(define renamed-builtins
  (hash 'inversesqrt "inverseSqrt" 'roundEven "round" 'faceforward "faceForward"
        'dFdx "dpdx" 'dFdy "dpdy" 'dFdxFine "dpdxFine" 'dFdyFine "dpdyFine" 'dFdxCoarse "dpdxCoarse"
        'dFdyCoarse "dpdyCoarse" 'fwidthFine "fwidthFine" 'fwidthCoarse "fwidthCoarse"))

;; Vector comparisons, which are operators in WGSL
(define comparison-builtins
  (hash 'lessThan "<" 'lessThanEqual "<=" 'greaterThan ">" 'greaterThanEqual ">=" 'equal "==" 'notEqual "!="))

;; Subgroup operations WGSL has under their GLSL names
(define wgsl-subgroup-builtins
  '(subgroupAdd subgroupMul subgroupMin subgroupMax subgroupAnd subgroupOr subgroupXor
    subgroupInclusiveAdd subgroupInclusiveMul subgroupExclusiveAdd subgroupExclusiveMul
    subgroupElect subgroupAll subgroupAny subgroupBroadcastFirst subgroupBallot subgroupShuffle subgroupShuffleXor))

(define (print-expr st e)
  (define (p e) (print-expr st e))
  (define (operand e) (wrap e (p e)))
  (define (args es) (string-join (map p es) ", "))
  (define stage (state-stage st))
  (cond
    [(and (memq (match e [(t-global k _ _ _ _) k] [(or (t-field _ _ _ _) (t-index _ _ _ _) (t-swizzle _ _ _ _)) 'place] [_ #f])
                '(binding shared place))
          (raw-place? st e))
     (define a (address-of e))
     (format "~a(~a)" (load-helper st (t-global-name (address-root a)) (texpr-type e)) (offset-text st a))]
    [else
     (match e
       [(t-lit v t _) (literal-text v t)]
       [(t-local name _ _) (wgsl-name name)]
       [(t-global kind name spelling t l)
        (case kind
          [(push) (set-state-push-used?! st #t) (format "zgalPush.~a" (wgsl-name name))]
          [(stage) (stage-variable st name l)]
          [(input) (wgsl-name spelling)]
          [(constant) (if (set-member? (state-function-constants st) name) (format "~a()" (wgsl-name name)) (wgsl-name name))]
          [else (wgsl-name name)])]
       [(t-call f as t l) (print-call st f as t l)]
       [(t-user-call f as _ _) (format "~a(~a)" (wgsl-name f) (args as))]
       [(t-texture op as t l) (print-texture st op as l)]
       [(t-op 'negate (list a) t _)
        (define text (operand a))
        (cond
          [(eq? (component-kind t) 'uint) (format "(~a(0u) - ~a)" (wgsl-type t) text)]
          ;; Not --x, which is a decrement
          [(regexp-match? #rx"^-" text) (format "-(~a)" text)]
          [else (format "-~a" text)])]
       [(t-op 'not (list a) _ _) (format "!~a" (operand a))]
       [(t-op 'bitnot (list a) _ _) (format "~~~a" (operand a))]
       ;; Only arrays of textures (texture arrays) reach here
       [(t-op 'array-length (list a) _ _) (format "textureNumLayers(~a)" (p a))]
       [(t-index (and b (t-global 'binding name _ (binding-array-t _) _)) _ _ l)
        (web-error l "~a's elements are layers of a texture array on the web, so they can only be used directly in texture operations (sample, sample-lod, fetch and texture-size)" name)]
       [(t-op (and op (or 'shl 'shr)) (list a b) _ _)
        (format "~a ~a ~a" (operand a) (if (eq? op 'shl) "<<" ">>")
                (match (texpr-type b)
                  [(scalar-t 'int) (format "u32(~a)" (p b))]
                  [(vector-t 'int n) (format "vec~au(~a)" n (p b))]
                  [_ (operand b)]))]
       [(t-op (and op (or '= 'not=)) (list a b) _ _)
        (define text (format "~a ~a ~a" (operand a) (if (eq? op '=) "==" "!=") (operand b)))
        (if (vector-t? (texpr-type a)) (format "~a(~a)" (if (eq? op '=) "all" "any") text) text)]
       [(t-op op as _ _)
        (string-join (map operand as)
                     (format " ~a " (hash-ref (hash '+ "+" '- "-" '* "*" '/ "/" 'remainder "%" 'bitand "&" 'bitor "|"
                                                    'bitxor "^" '< "<" '<= "<=" '> ">" '>= ">=" 'and "&&" 'or "||")
                                              op)))]
       [(t-construct as t l) (print-construct st as t l)]
       [(t-field b name _ _) (format "~a.~a" (operand b) (wgsl-name name))]
       [(t-swizzle b comps _ _) (format "~a.~a" (operand b) comps)]
       [(t-index b i _ _) (format "~a[~a]" (operand b) (p i))]
       [(t-if c a b _ _) (format "select(~a, ~a, ~a)" (p b) (p a) (p c))]
       [(t-store i texel v _) (format "textureStore(~a, ~a, ~a)" (p i) (p texel) (p v))]
       [(t-barrier _ _) "workgroupBarrier()"]
       [(t-exit op _) (symbol->string op)]
       [(t-atomic op place as t l) (print-atomic st op place as t l)])]))

;; A stage variable, as the entry point's built-in parameters give it
(define (stage-variable st name where)
  (define (builtin b type) (entry-builtin! st b type))
  (define (subgroups!) (set-state-subgroups?! st #t))
  (match name
    ["vertexIndex" (format "i32(~a)" (builtin "vertex_index" "u32"))]
    ["instanceIndex" (format "i32(~a)" (builtin "instance_index" "u32"))]
    ["fragCoord" (builtin "position" "vec4f")]
    ["frontFacing" (builtin "front_facing" "bool")]
    ["globalInvocationId" (builtin "global_invocation_id" "vec3u")]
    ["localInvocationId" (builtin "local_invocation_id" "vec3u")]
    ["workgroupId" (builtin "workgroup_id" "vec3u")]
    ["workgroupCount" (builtin "num_workgroups" "vec3u")]
    ["subgroupSize" (subgroups!) (lane-private! st "zgalSubgroupSize" "subgroup_size")]
    ["subgroupInvocationId" (subgroups!) (lane-private! st "zgalSubgroupInvocationId" "subgroup_invocation_id")]
    ;; WGSL has no subgroup ids: invocations make subgroups in order of their local index
    ["subgroupId"
     (subgroups!)
     (format "(~a / ~a)" (builtin "local_invocation_index" "u32") (lane-private! st "zgalSubgroupSize" "subgroup_size"))]
    ["subgroupCount"
     (subgroups!)
     (match-define (list x y z) (material-workgroup (state-material st)))
     (format "((~au + ~a - 1u) / ~a)" (* x y z) (lane-private! st "zgalSubgroupSize" "subgroup_size")
             (lane-private! st "zgalSubgroupSize" "subgroup_size"))]))

;; A built-in kept in a private variable (set first thing in main), so helpers can read it too
(define (lane-private! st variable builtin)
  (private! st (format "~a: u32" variable))
  (entry-builtin! st builtin "u32" (string-append variable "In"))
  variable)

(define (print-call st f as t where)
  (define (p e) (print-expr st e))
  (define (args es) (string-join (map p es) ", "))
  ;; A scalar argument where WGSL wants the vector type of the result
  (define (splat e)
    (if (and (vector-t? t) (scalar-t? (texpr-type e))) (format "~a(~a)" (wgsl-type t) (p e)) (p e)))
  (cond
    [(hash-ref renamed-builtins f #f) => (λ (n) (format "~a(~a)" n (args as)))]
    [(hash-ref comparison-builtins f #f)
     => (λ (op) (format "(~a ~a ~a)" (wrap (first as) (p (first as))) op (wrap (second as) (p (second as)))))]
    [(memq f wgsl-subgroup-builtins)
     (set-state-subgroups?! st #t)
     (format "~a(~a)" f (args as))]
    [else
     (case f
       [(not) (format "!~a" (wrap (first as) (p (first as))))]
       [(atan) (if (= (length as) 2) (format "atan2(~a)" (args as)) (format "atan(~a)" (args as)))]
       [(min max) (format "~a(~a, ~a)" f (p (first as)) (splat (second as)))]
       [(clamp) (format "clamp(~a, ~a, ~a)" (p (first as)) (splat (second as)) (splat (third as)))]
       [(step) (format "step(~a, ~a)" (splat (first as)) (p (second as)))]
       [(smoothstep) (format "smoothstep(~a, ~a, ~a)" (splat (first as)) (splat (second as)) (p (third as)))]
       [(mix)
        (if (vector-t? (texpr-type (third as)))
            (if (eq? (vector-t-kind (texpr-type (third as))) 'bool)
                (format "select(~a)" (args as))
                (format "mix(~a)" (args as)))
            (format "mix(~a, ~a, ~a)" (p (first as)) (p (second as)) (p (third as))))]
       [(mod)
        (define h (helper! st (format "zgalMod_~a" (type-key t))
                    (λ () (format "fn zgalMod_~a(x: ~a, y: ~a) -> ~a {\n    return x - y * floor(x / y);\n}"
                                  (type-key t) (wgsl-type t) (wgsl-type t) (wgsl-type t)))))
        (format "~a(~a, ~a)" h (p (first as)) (splat (second as)))]
       [(floatBitsToUint floatBitsToInt) (format "bitcast<~a>(~a)" (wgsl-type t) (args as))]
       [(uintBitsToFloat intBitsToFloat)
        ;; WGSL evaluates constant expressions when compiling, and a constant's bits giving an
        ;; infinity or NaN is an error there: a function call makes it a runtime value
        (define arg (first as))
        (if (constant-expression? arg)
            (format "bitcast<~a>(~a(~a))" (wgsl-type t)
                    (helper! st (format "zgalRuntime_~a" (type-key (texpr-type arg)))
                      (λ () (format "fn zgalRuntime_~a(x: ~a) -> ~a {\n    return x;\n}"
                                    (type-key (texpr-type arg)) (wgsl-type (texpr-type arg)) (wgsl-type (texpr-type arg)))))
                    (p arg))
            (format "bitcast<~a>(~a)" (wgsl-type t) (args as)))]
       [(inverse) (format "~a(~a)" (inverse-helper st (matrix-t-size t)) (args as))]
       [(memoryBarrierBuffer) "storageBarrier()"]
       [(subgroupAllEqual)
        (set-state-subgroups?! st #t)
        (define a (p (first as)))
        (if (vector-t? (texpr-type (first as)))
            (format "subgroupAll(all(~a == subgroupBroadcastFirst(~a)))" a a)
            (format "subgroupAll(~a == subgroupBroadcastFirst(~a))" a a))]
       [(subgroupBallotBitCount)
        (format "~a(~a)" (helper! st "zgalBallotCount"
                           (λ () "fn zgalBallotCount(b: vec4u) -> u32 {\n    let c = countOneBits(b);\n    return c.x + c.y + c.z + c.w;\n}"))
                (args as))]
       [(subgroupBallotInclusiveBitCount subgroupBallotExclusiveBitCount)
        (set-state-subgroups?! st #t)
        (define lane (lane-private! st "zgalSubgroupInvocationId" "subgroup_invocation_id"))
        (define inclusive? (eq? f 'subgroupBallotInclusiveBitCount))
        (define name (if inclusive? "zgalBallotInclusiveCount" "zgalBallotExclusiveCount"))
        (format "~a(~a)"
                (helper! st name
                  (λ ()
                    (format (string-append
                             "fn ~a(b: vec4u) -> u32 {\n"
                             "    // The bits of invocations before this one~a\n"
                             "    let n = ~a~a;\n"
                             "    var mask = vec4u(0u);\n"
                             "    for (var i = 0u; i < 4u; i += 1u) {\n"
                             "        let low = i * 32u;\n"
                             "        if (n >= low + 32u) { mask[i] = 0xffffffffu; } else if (n > low) { mask[i] = (1u << (n - low)) - 1u; }\n"
                             "    }\n"
                             "    let c = countOneBits(b & mask);\n"
                             "    return c.x + c.y + c.z + c.w;\n"
                             "}")
                            name (if inclusive? ", and its own" "") lane (if inclusive? " + 1u" ""))))
                (args as))]
       [(subgroupInclusiveMin subgroupInclusiveMax subgroupInclusiveAnd subgroupInclusiveOr subgroupInclusiveXor
         subgroupExclusiveMin subgroupExclusiveMax subgroupExclusiveAnd subgroupExclusiveOr subgroupExclusiveXor)
        (format "~a(~a)" (scan-helper st f t) (args as))]
       [else
        (when (gpu-only-function? f) (set-state-subgroups?! st (or (state-subgroups? st) (and (subgroup-category f) #t))))
        (format "~a(~a)" f (args as))])]))

;; Whether e is made only of literals and constants
(define (constant-expression? e)
  (for/and ([x (in-list (subexpressions e))])
    (match x
      [(t-local _ _ _) #f]
      [(t-global kind _ _ _ _) (eq? kind 'constant)]
      [(or (t-user-call _ _ _ _) (t-texture _ _ _ _) (t-atomic _ _ _ _ _)) #f]
      [_ #t])))

;; A subgroup scan WGSL lacks (it has adds and multiplies), by shuffles: each step combines the
;; value d invocations before
(define (scan-helper st f t)
  (set-state-subgroups?! st #t)
  (define name (format "zgal_~a_~a" f (type-key t)))
  (define lane (lane-private! st "zgalSubgroupInvocationId" "subgroup_invocation_id"))
  (define size (lane-private! st "zgalSubgroupSize" "subgroup_size"))
  (define s (symbol->string f))
  (define exclusive? (regexp-match? #rx"Exclusive" s))
  (define op (string-downcase (cadr (regexp-match #rx"clusive(.*)$" s))))
  (define (combine a b)
    (case op
      [("min") (format "min(~a, ~a)" a b)] [("max") (format "max(~a, ~a)" a b)]
      [("and") (format "(~a & ~a)" a b)] [("or") (format "(~a | ~a)" a b)] [("xor") (format "(~a ^ ~a)" a b)]))
  (define kind (component-kind t))
  (define identity
    (case op
      [("min") (case kind [(float) "3.40282347e38f"] [(int) "2147483647i"] [(uint) "0xffffffffu"] [(bool) "true"])]
      [("max") (case kind [(float) "-3.40282347e38f"] [(int) "(-2147483647i - 1i)"] [(uint) "0u"] [(bool) "false"])]
      [("and") (case kind [(int) "-1i"] [(uint) "0xffffffffu"] [(bool) "true"])]
      [else (case kind [(int) "0i"] [(uint) "0u"] [(bool) "false"])]))
  (define identity-value (if (vector-t? t) (format "~a(~a)" (wgsl-type t) identity) identity))
  (define (bool-combine a b)
    (case op [("min" "and") (format "(~a && ~a)" a b)] [("max" "or") (format "(~a || ~a)" a b)] [("xor") (format "(~a != ~a)" a b)]))
  (define step (if (eq? kind 'bool) bool-combine combine))
  (helper! st name
    (λ ()
      (format (string-append
               "fn ~a(value: ~a) -> ~a {\n"
               "    var x = ~a;\n"
               "    for (var d = 1u; d < ~a; d *= 2u) {\n"
               "        let y = subgroupShuffleUp(x, d);\n"
               "        if (~a >= d) { x = ~a; }\n"
               "    }\n"
               "    return x;\n"
               "}")
              name (wgsl-type t) (wgsl-type t)
              (if exclusive? (format "subgroupShuffleUp(value, 1u)") "value")
              size lane (step "x" "y"))))
  (if exclusive?
      ;; The first invocation's exclusive scan is the identity
      (helper! st (string-append name "_exclusive")
        (λ ()
          (format "fn ~a_exclusive(value: ~a) -> ~a {\n    return select(~a(value), ~a, ~a == 0u);\n}"
                  name (wgsl-type t) (wgsl-type t) name identity-value lane)))
      name))

;; WGSL has no matrix inverse
(define (inverse-helper st n)
  (define name (format "zgalInverse~a" n))
  (helper! st name
    (λ ()
      (case n
        [(2) (string-append
              "fn zgalInverse2(m: mat2x2f) -> mat2x2f {\n"
              "    return mat2x2f(m[1][1], -m[0][1], -m[1][0], m[0][0]) * (1.0f / determinant(m));\n"
              "}")]
        [(3) (string-append
              "fn zgalInverse3(m: mat3x3f) -> mat3x3f {\n"
              "    let a = cross(m[1], m[2]);\n"
              "    let b = cross(m[2], m[0]);\n"
              "    let c = cross(m[0], m[1]);\n"
              "    return transpose(mat3x3f(a, b, c)) * (1.0f / dot(m[0], a));\n"
              "}")]
        [(4) (string-append
              "fn zgalInverse4(m: mat4x4f) -> mat4x4f {\n"
              "    let a00 = m[0][0]; let a01 = m[0][1]; let a02 = m[0][2]; let a03 = m[0][3];\n"
              "    let a10 = m[1][0]; let a11 = m[1][1]; let a12 = m[1][2]; let a13 = m[1][3];\n"
              "    let a20 = m[2][0]; let a21 = m[2][1]; let a22 = m[2][2]; let a23 = m[2][3];\n"
              "    let a30 = m[3][0]; let a31 = m[3][1]; let a32 = m[3][2]; let a33 = m[3][3];\n"
              "    let b00 = a00 * a11 - a01 * a10; let b01 = a00 * a12 - a02 * a10;\n"
              "    let b02 = a00 * a13 - a03 * a10; let b03 = a01 * a12 - a02 * a11;\n"
              "    let b04 = a01 * a13 - a03 * a11; let b05 = a02 * a13 - a03 * a12;\n"
              "    let b06 = a20 * a31 - a21 * a30; let b07 = a20 * a32 - a22 * a30;\n"
              "    let b08 = a20 * a33 - a23 * a30; let b09 = a21 * a32 - a22 * a31;\n"
              "    let b10 = a21 * a33 - a23 * a31; let b11 = a22 * a33 - a23 * a32;\n"
              "    let det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;\n"
              "    return mat4x4f(\n"
              "        a11 * b11 - a12 * b10 + a13 * b09, a02 * b10 - a01 * b11 - a03 * b09,\n"
              "        a31 * b05 - a32 * b04 + a33 * b03, a22 * b04 - a21 * b05 - a23 * b03,\n"
              "        a12 * b08 - a10 * b11 - a13 * b07, a00 * b11 - a02 * b08 + a03 * b07,\n"
              "        a32 * b02 - a30 * b05 - a33 * b01, a20 * b05 - a22 * b02 + a23 * b01,\n"
              "        a10 * b10 - a11 * b08 + a13 * b06, a01 * b08 - a00 * b10 - a03 * b06,\n"
              "        a30 * b04 - a31 * b02 + a33 * b00, a21 * b02 - a20 * b04 - a23 * b00,\n"
              "        a11 * b07 - a10 * b09 - a12 * b06, a00 * b09 - a01 * b07 + a02 * b06,\n"
              "        a31 * b01 - a30 * b03 - a32 * b00, a20 * b03 - a21 * b01 + a22 * b00) * (1.0f / det);\n"
              "}")]))))

(define (print-texture st op as where)
  (define (p e) (print-expr st e))
  (define fragment? (eq? (state-stage st) 'fragment))
  (define depth (state-depth st))
  (define (depth? e) (set-member? depth (opaque-name e)))
  (when (and (memq op '(sample sample-lod)) (depth? (second as)))
    (web-error where "sampler ~a also compares depths, and WebGPU's comparison samplers only compare: use another sampler here"
               (opaque-name (second as))))
  ;; Sampling a depth texture gives its depth, which Vulkan gives as (depth, 0, 0, 1)
  (define (depth-texel t text) (if (depth? t) (format "vec4f(~a, 0.0f, 0.0f, 1.0f)" text) text))
  ;; An element of an array of textures: a texture array and its layer
  (define layer
    (match (first as)
      [(t-index (t-global 'binding _ _ (binding-array-t _) _) i _ _) i]
      [_ #f]))
  (define (layer-arg) (if layer (format ", ~a" (p layer)) ""))
  (define (p-texture t) (if layer (p (t-index-base t)) (p t)))
  (match* (op as)
    [((or 'image-load 'image-size 'sample-compare) _)
     #:when layer
     (web-error where "~a can't be used on an element of an array on the web" op)]
    [('sample (list t s uv))
     #:when layer
     (if fragment?
         (format "textureSample(~a, ~a, ~a~a)" (p-texture t) (p s) (p uv) (layer-arg))
         (format "textureSampleLevel(~a, ~a, ~a~a, 0.0f)" (p-texture t) (p s) (p uv) (layer-arg)))]
    [('sample-lod (list t s uv lod))
     #:when layer
     (format "textureSampleLevel(~a, ~a, ~a~a, ~a)" (p-texture t) (p s) (p uv) (layer-arg) (p lod))]
    [('fetch (list t _ texel lod))
     #:when layer
     (format "textureLoad(~a, ~a~a, ~a)" (p-texture t) (p texel) (layer-arg) (p lod))]
    [('texture-size (list t lod))
     #:when layer
     (format "vec2i(textureDimensions(~a, ~a))" (p-texture t) (p lod))]
    [('sample (list t s uv))
     (depth-texel t (if fragment?
                  (format "textureSample(~a, ~a, ~a)" (p t) (p s) (p uv))
                  (format "textureSampleLevel(~a, ~a, ~a, 0~a)" (p t) (p s) (p uv) (if (depth? t) "i" ".0f"))))]
    [('sample-lod (list t s uv lod))
     (depth-texel t (format "textureSampleLevel(~a, ~a, ~a, ~a)" (p t) (p s) (p uv) (if (depth? t) (format "i32(~a)" (p lod)) (p lod))))]
    [('fetch (list t _ texel lod)) (depth-texel t (format "textureLoad(~a, ~a, ~a)" (p t) (p texel) (p lod)))]
    [('sample-compare (list t s uv ref))
     #:when (not (depth? s))
     (web-error where "sampler ~a is also used to sample, and WebGPU's samplers either sample or compare depths: use another sampler here"
                (opaque-name s))]
    [('sample-compare (list t s uv ref))
     (if fragment?
         (format "textureSampleCompare(~a, ~a, ~a, ~a)" (p t) (p s) (p uv) (p ref))
         (format "textureSampleCompareLevel(~a, ~a, ~a, ~a)" (p t) (p s) (p uv) (p ref)))]
    [('texture-size (list t lod)) (format "vec2i(textureDimensions(~a, ~a))" (p t) (p lod))]
    [('image-load (list i texel)) (format "textureLoad(~a, ~a)" (p i) (p texel))]
    [('image-size (list i)) (format "vec2i(textureDimensions(~a))" (p i))]))

(define (print-construct st as t where)
  (define (p e) (print-expr st e))
  (match t
    [(scalar-t k) #:when (small-integer-kind? k)
     ;; Narrowing keeps the low bits, as a value of the 32-bit kind
     (define a (first as))
     (define wide (if (eq? (component-kind (texpr-type a)) (widened-kind k)) (wrap a (p a)) (format "~a(~a)" (scalar-name k) (p a))))
     (case k
       [(uint8) (format "(~a & 0xffu)" wide)]
       [(uint16) (format "(~a & 0xffffu)" wide)]
       [(int8) (format "((~a << 24u) >> 24u)" wide)]
       [(int16) (format "((~a << 16u) >> 16u)" wide)])]
    ;; A literal converted is a literal of the type (an int literal may only fit a uint)
    [(scalar-t k) #:when (and (t-lit? (first as)) (exact-integer? (t-lit-value (first as))) (memq k '(int uint float)))
     (literal-text (t-lit-value (first as)) t)]
    [(scalar-t k) (format "~a(~a)" (scalar-name k) (p (first as)))]
    ;; A vector of another kind converted
    [(vector-t k n) #:when (and (= (length as) 1) (vector-t? (texpr-type (first as))))
     (format "~a(~a)" (wgsl-type t) (print-expr st (first as)))]
    [(vector-t k n)
     ;; WGSL doesn't convert components: convert each argument of another kind
     (format "~a(~a)" (wgsl-type t)
             (string-join
              (for/list ([a (in-list as)])
                (match (texpr-type a)
                  [(scalar-t ak) #:when (not (eq? (widened-kind ak) k)) (format "~a(~a)" (scalar-name k) (p a))]
                  [(vector-t ak an) #:when (not (eq? ak k)) (format "~a(~a)" (wgsl-type (vector-t k an)) (p a))]
                  [_ (p a)]))
              ", "))]
    [(matrix-t n)
     (match as
       [(list a) #:when (scalar-t? (texpr-type a))
        ;; A diagonal matrix
        (define v (p a))
        (format "~a(~a)" (wgsl-type t)
                (string-join (for*/list ([c (in-range n)] [r (in-range n)]) (if (= c r) v "0.0f")) ", "))]
       [(list a) #:when (matrix-t? (texpr-type a))
        (define from (matrix-t-size (texpr-type a)))
        (define v (p a))
        (if (= from n)
            v
            ;; Columns cut down or extended with the identity's
            (format "~a(~a)" (wgsl-type t)
                    (string-join
                     (for*/list ([c (in-range n)] [r (in-range n)])
                       (if (and (< c from) (< r from)) (format "~a[~a][~a]" (wrap a v) c r) (if (= c r) "1.0f" "0.0f")))
                     ", ")))]
       [_ (format "~a(~a)" (wgsl-type t) (string-join (map p as) ", "))])]
    [(struct-t d) (format "~a(~a)" (wgsl-type t) (string-join (map p as) ", "))]
    [(array-t e n) (format "~a(~a)" (wgsl-type t) (string-join (map p as) ", "))]
    [_ (web-error where "~a values are only in apps" (type-name t))]))

;; An atomic operation on a raw place, a word of a raw variable
(define (print-atomic st op place as t where)
  (define a (address-of place))
  (define var (t-global-name (address-root a)))
  (define index (format "(~a) >> 2u" (offset-text st a)))
  (define pointer (format "&~a[~a]" (wgsl-name var) index))
  (define kind (scalar-t-kind t))
  (define (p e) (print-expr st e))
  (define (bits e) (scalar-bits kind (p e)))
  (define (from-bits text) (case kind [(uint) text] [(int) (format "bitcast<i32>(~a)" text)] [(float) (format "bitcast<f32>(~a)" text)]))
  (match* (op kind)
    [((or 'add 'and 'or 'xor 'exchange) (or 'uint 'int))
     (from-bits (format "~a(~a, ~a)"
                        (case op [(add) "atomicAdd"] [(and) "atomicAnd"] [(or) "atomicOr"] [(xor) "atomicXor"] [(exchange) "atomicExchange"])
                        pointer (bits (first as))))]
    [((or 'min 'max) 'uint) (format "~a(~a, ~a)" (if (eq? op 'min) "atomicMin" "atomicMax") pointer (p (first as)))]
    [('exchange 'float) (from-bits (format "atomicExchange(~a, ~a)" pointer (bits (first as))))]
    [('compare-exchange _)
     ;; GLSL's compare-exchange doesn't fail spuriously; WGSL's weak one may, so it's retried
     (define h (helper! st (format "zgalCompareExchange_~a" var)
                 (λ ()
                   (format (string-append
                            "fn zgalCompareExchange_~a(i: u32, comparand: u32, value: u32) -> u32 {\n"
                            "    loop {\n"
                            "        let r = atomicCompareExchangeWeak(&~a[i], comparand, value);\n"
                            "        if (r.exchanged || r.old_value != comparand) { return r.old_value; }\n"
                            "    }\n"
                            "}")
                           var (wgsl-name var)))))
     (from-bits (format "~a(~a, ~a, ~a)" h index (bits (first as)) (bits (second as))))]
    [(_ _)
     ;; Signed minimums and maximums and float adds: a compare-exchange loop
     (define name (format "zgalAtomic_~a_~a_~a" op kind var))
     (define h (helper! st name
                 (λ ()
                   (define ty (scalar-name kind))
                   (define (val x) (case kind [(int) (format "bitcast<i32>(~a)" x)] [(float) (format "bitcast<f32>(~a)" x)]))
                   (format (string-append
                            "fn ~a(i: u32, value: ~a) -> ~a {\n"
                            "    var old = atomicLoad(&~a[i]);\n"
                            "    loop {\n"
                            "        let r = atomicCompareExchangeWeak(&~a[i], old, bitcast<u32>(~a));\n"
                            "        if (r.exchanged) { return ~a; }\n"
                            "        old = r.old_value;\n"
                            "    }\n"
                            "}")
                           name ty ty (wgsl-name var) (wgsl-name var)
                           (case op [(add) (format "~a + value" (val "old"))] [(min) (format "min(~a, value)" (val "old"))] [(max) (format "max(~a, value)" (val "old"))])
                           (val "old")))))
     (format "~a(~a, ~a)" h index (p (first as)))]))

;; ---------------------------------------------------------------------------------------------
;; Statements

;; If expressions WGSL's select can't give, or whose branches are worth evaluating only when taken
(define (if-as-statements? e)
  (match e
    [(t-if _ a b t _)
     (or (not (or (scalar-t? t) (vector-t? t)))
         (for/or ([x (in-list (append (subexpressions a) (subexpressions b)))])
           (or (t-user-call? x) (t-texture? x) (t-call? x) (t-index? x))))]
    [_ #f]))

(define (lower-wgsl e fresh)
  (parameterize ([lower-if-as-statements? if-as-statements?])
    (lower e fresh)))

(define (statement-lines st stmts indent)
  (define (p e) (print-expr st e))
  (append*
   (for/list ([s (in-list stmts)])
     (define (line fmt . args) (list (string-append indent (apply format fmt args))))
     (define inner (string-append indent "    "))
     (match s
       [(s-declare name t v _)
        (if v
            (line "var ~a: ~a = ~a;" (wgsl-name name) (wgsl-type t) (p v))
            (line "var ~a: ~a;" (wgsl-name name) (wgsl-type t)))]
       [(s-assign place v _)
        (cond
          [(raw-place? st place)
           (define a (address-of place))
           (define var (t-global-name (address-root a)))
           (line "~a(~a, ~a);" (store-helper st var (texpr-type place) (= (address-alignment place) 4)) (offset-text st a) (p v))]
          [else (line "~a = ~a;" (p place) (p v))])]
       [(s-if c then else _)
        (append (line "if (~a) {" (p c))
                (statement-lines st then inner)
                (if (null? else)
                    (line "}")
                    (append (line "} else {") (statement-lines st else inner) (line "}"))))]
       [(s-for name t from to step body _)
        (append (line "for (var ~a: ~a = ~a; ~a < ~a; ~a += ~a) {" (wgsl-name name) (wgsl-type t) (p from)
                      (wgsl-name name) (p to) (wgsl-name name) (p step))
                (statement-lines st body inner)
                (line "}"))]
       [(s-effect e _)
        (match e
          [(t-exit op _) (line "~a;" op)]
          [_ (line "~a;" (p e))])]
       [(s-while cs c body _)
        (if (null? cs)
            (append (line "while (~a) {" (p c)) (statement-lines st body inner) (line "}"))
            (append (line "loop {")
                    (statement-lines st cs inner)
                    (list (format "~aif (!(~a)) {" inner (p c)) (format "~a    break;" inner) (format "~a}" inner))
                    (statement-lines st body inner)
                    (line "}")))]))))

(define (fresh-maker)
  (define n 0)
  (λ (base)
    (set! n (add1 n))
    (format "zgal~a~a" (string-titlecase base) n)))

;; A function's parameter types: textures and samplers it compares depths with are depth ones
(define (parameter-type st f name t)
  (define depth? (set-member? (depth-names (function-decl-body f) (state-functions st)) name))
  (match t
    [(opaque-t 'texture2D) (if depth? "texture_depth_2d" "texture_2d<f32>")]
    [(opaque-t 'sampler) (if depth? "sampler_comparison" "sampler")]
    [(opaque-t k) (web-error (function-decl-loc f) "~a's parameter ~a is ~a, which WGSL functions can't take" (function-decl-name f) name k)]
    [_ (wgsl-type t (function-decl-loc f))]))

(define (emit-function st f)
  ;; Its textures and samplers comparing depths are its parameters
  (set-state-depth! st (depth-names (function-decl-body f) (state-functions st)))
  (define-values (stmts value) (lower-wgsl (function-decl-body f) (fresh-maker)))
  (define result (wgsl-type (function-decl-result f) (function-decl-loc f)))
  (append
   (list ""
         (format "fn ~a(~a)~a {" (wgsl-name (function-decl-name f))
                 (string-join (for/list ([p (in-list (function-decl-params f))])
                                (format "~a: ~a" (wgsl-name (car p)) (parameter-type st f (car p) (cdr p))))
                              ", ")
                 (if result (format " -> ~a" result) "")))
   (statement-lines st stmts "    ")
   (if value (list (format "    return ~a;" (print-expr st value))) '())
   (list "}")))

;; A constant: a WGSL constant if WGSL can compute it when compiling, else a function giving it
(define (emit-constant st c)
  (define-values (stmts value) (lower-wgsl (constant-decl-body c) (fresh-maker)))
  (define name (wgsl-name (constant-decl-name c)))
  (define type (wgsl-type (constant-decl-type c) (constant-decl-loc c)))
  (cond
    [(and (null? stmts) (wgsl-constant? st (constant-decl-body c)))
     (list "" (format "const ~a: ~a = ~a;" name type (print-expr st value)))]
    [else
     (set-state-function-constants! st (set-add (state-function-constants st) (constant-decl-name c)))
     (append (list "" (format "fn ~a() -> ~a {" name type))
             (statement-lines st stmts "    ")
             (list (format "    return ~a;" (print-expr st value)) "}"))]))

;; Whether WGSL can compute e when compiling: no calls of functions, or bitcasts to floats (whose
;; arguments are kept from it, see print-call), or constants given by functions
(define (wgsl-constant? st e)
  (for/and ([x (in-list (subexpressions e))])
    (match x
      [(or (t-user-call _ _ _ _) (t-call (or 'uintBitsToFloat 'intBitsToFloat 'inverse 'mod) _ _ _)) #f]
      [(t-global 'constant name _ _ _) (not (set-member? (state-function-constants st) name))]
      [_ #t])))

(define (interpolation v)
  (case (varying-interpolation v)
    [(flat) " @interpolate(flat)"]
    [(noperspective) " @interpolate(linear)"]
    [else ""]))

;; The entry point, main
(define (emit-main st s body)
  (set-state-depth! st (usage-depth (state-usage st)))
  (define m (state-material st))
  (define-values (stmts results) (lower-wgsl body (fresh-maker)))
  (define body-lines (statement-lines st stmts "    "))
  (define stage (state-stage st))
  (define result-pairs (if (list? results) results '()))
  (define (result-lines target)
    (append*
     (for/list ([r (in-list result-pairs)])
       (define position? (string=? (car r) "position"))
       (cons (format "    ~a.~a = ~a;" target (if position? "zgalPosition" (wgsl-name (car r))) (print-expr st (cdr r)))
             ;; ZGAL's clip space is Vulkan's, whose y points down; WebGPU's points up
             (if position?
                 (list (format "    ~a.zgalPosition.y = -~a.zgalPosition.y;" target target))
                 '())))))
  ;; Results are printed before the parameters are collected: they may use built-ins
  (define (finish header-params return-type output-struct output-lines)
    (define builtin-params
      (for/list ([b (in-list (state-entry-builtins st))])
        (match-define (list builtin name type) b)
        (format "@builtin(~a) ~a: ~a" builtin name type)))
    ;; Built-ins helpers read are kept in private variables
    (define private-sets
      (for/list ([b (in-list (state-entry-builtins st))] #:when (regexp-match? #rx"In$" (second b)))
        (format "    ~a = ~a;" (regexp-replace #rx"In$" (second b) "") (second b))))
    (append
     output-struct
     (list ""
           (string-append
            (case stage
              [(compute) (match-define (list x y z) (material-workgroup m)) (format "@compute @workgroup_size(~a, ~a, ~a)\n" x y z)]
              [(vertex) "@vertex\n"]
              [(fragment) "@fragment\n"])
            (format "fn main(~a)~a {" (string-join (append header-params builtin-params) ", ")
                    (if return-type (format " -> ~a" return-type) ""))))
     private-sets
     body-lines
     output-lines
     (list "}")))
  (match stage
    ['compute (finish '() #f '() '())]
    ['vertex
     (define input (material-vertex-input m))
     (define params
       (if input
           (for/list ([entry (in-list (vertex-input-attributes input))])
             (match-define (cons index a) entry)
             (format "@location(~a) ~a: ~a" index (wgsl-name (input-variable-name (field-name a))) (wgsl-type (field-type a))))
           '()))
     (define outputs (result-lines "zgalOut"))
     (finish params "ZgalVertexOutput"
             (append (list "" "struct ZgalVertexOutput {" "    @builtin(position) zgalPosition: vec4f,")
                     (for/list ([v (in-list (material-varyings m))] [location (in-naturals)])
                       (format "    @location(~a)~a ~a: ~a," location (interpolation v) (wgsl-name (varying-name v)) (wgsl-type (varying-type v))))
                     (list "}"))
             (append (list "    var zgalOut: ZgalVertexOutput;") outputs (list "    return zgalOut;")))]
    ['fragment
     (define params
       (for/list ([v (in-list (material-varyings m))] [location (in-naturals)])
         (format "@location(~a)~a ~a: ~a" location (interpolation v) (wgsl-name (varying-name v)) (wgsl-type (varying-type v)))))
     (define outputs (result-lines "zgalOut"))
     (if (null? (material-outputs m))
         (finish params #f '() '())
         (finish params "ZgalFragmentOutput"
                 (append (list "" "struct ZgalFragmentOutput {")
                         (for/list ([o (in-list (material-outputs m))] [location (in-naturals)])
                           (format "    @location(~a) ~a: ~a," location (wgsl-name (field-name o)) (wgsl-type (field-type o))))
                         (list "}"))
                 (append (list "    var zgalOut: ZgalFragmentOutput;") outputs (list "    return zgalOut;"))))]))
