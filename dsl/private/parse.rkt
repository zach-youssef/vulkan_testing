#lang racket/base
;; Reads a .zgal file and checks it, producing the program (ast.rkt). Every problem is reported
;; as a catalogued error (errors.rkt) at the syntax responsible.

(require racket/list
         racket/match
         racket/path
         racket/runtime-path
         racket/file
         racket/set
         racket/string
         "app.rkt"
         "ast.rkt"
         "errors.rkt"
         "body.rkt"
         "builtins.rkt"
         "graph.rkt"
         "layout.rkt"
         "main.rkt"
         "names.rkt"
         "syntax.rkt")

(provide read-forms
         parse-program
         push-constant-limit
         library-directory)

;; The standard library's modules, which (import name) finds
(define-runtime-path library-directory "../lib")

;; The wrapper's pipeline layout has 128 bytes of push constants (DescriptorHeap::kPushConstantSize;
;; the generated C++ checks they agree)
(define push-constant-limit 128)

;; Reads every form in `in`. The reader is plain s-expressions: #lang and #reader, which would run
;; code, are refused.
(define (read-forms source-name in)
  (port-count-lines! in)
  (parameterize ([read-accept-reader #f]
                 [read-accept-lang #f]
                 [read-accept-compiled #f])
    (let loop ([acc '()])
      (define stx
        (with-handlers ([exn:fail:read? (λ (e) (raise-read-error e source-name))])
          (read-syntax source-name in)))
      (if (eof-object? stx) (reverse acc) (loop (cons stx acc))))))

(define (raise-read-error e source-name)
  (define where
    (match (exn:fail:read-srclocs e)
      [(cons s _) (loc source-name (or (srcloc-line s) 1) (add1 (or (srcloc-column s) 0)))]
      [_ (loc source-name 1 1)]))
  ;; The reader's message repeats the location: keep what follows "read-syntax: "
  (define message
    (match (regexp-match #px"read-syntax: (.*)$" (exn-message e))
      [(list _ m) m]
      [_ (exn-message e)]))
  (zgal-error 'read-error where "~a" message))

(define (hash-union* a b)
  (for/fold ([h a]) ([(k v) (in-hash b)]) (hash-set h k v)))

(define builtin-types
  (hash-union*
   (hash "float" (scalar-t 'float) "int" (scalar-t 'int) "uint" (scalar-t 'uint) "bool" (scalar-t 'bool)
         "uint8" (scalar-t 'uint8) "int8" (scalar-t 'int8) "uint16" (scalar-t 'uint16) "int16" (scalar-t 'int16)
         "mat2" (matrix-t 2) "mat3" (matrix-t 3) "mat4" (matrix-t 4))
   (for*/hash ([kind '(float int uint bool)] [n '(2 3 4)])
     (define t (vector-t kind n))
     (values (type-name t) t))))


;; ---------------------------------------------------------------------------------------------
;; The program

;; forms: the syntax read from the file at `source` (its path, as given)
(define (parse-program forms source)
  ;; The file's name names its C++ namespace
  (let ([ns (source-namespace source)])
    (unless (valid-identifier? ns)
      (zgal-error 'bad-identifier (loc source 1 1)
                  "the file's name, ~a, names its generated C++'s namespace, so it's letters, digits and underscores, starting with a letter: rename the file"
                  ns))
    (define reason (reserved-name-reason ns))
    (when reason
      (zgal-error 'reserved-name (loc source 1 1) "the file's name, ~a, names its generated C++'s namespace, but it ~a: rename the file" ns reason)))
  (define directory
    (let-values ([(dir _name _must-be-dir) (split-path source)])
      (if (path? dir) dir (string->path "."))))
  (define top-level (make-namespace "at the top level"))
  (define structs (make-hash))          ; name -> struct-decl
  (define formats (make-hash))          ; name -> vertex-format
  (define materials (make-hash))        ; name -> material
  (define shader-names (make-hash))     ; file name -> (cons material-name loc)
  ;; Every struct's name, so using one before its declaration can say so
  (define all-struct-names
    (for*/hash ([form (in-list forms)]
                #:when (eq? (head-symbol form) 'struct)
                [items (in-value (syntax->list form))]
                #:when (and (>= (length items) 2) (symbol? (syntax-e (cadr items)))))
      (values (symbol->string (syntax-e (cadr items))) #t)))

  (define (parse-type stx)
    (define d (syntax-e stx))
    (cond
      [(symbol? d)
       (define s (symbol->string d))
       (cond
         [(hash-ref builtin-types s #f)]
         [(hash-ref structs s #f) => struct-t]
         [(hash-ref all-struct-names s #f)
          (zgal-error 'unknown-type stx "struct ~a is used before it's declared; declare it earlier in the file" s)]
         [else (zgal-error 'unknown-type stx "unknown type ~a" s)])]
      [(eq? (head-symbol stx) 'array)
       (match (syntax->list stx)
         [(list _ element length)
          (define n (syntax-e length))
          (unless (exact-positive-integer? n)
            (zgal-error 'bad-array-length length "an array's length is a positive integer, not ~a" (show length)))
          (array-t (parse-type element) n)]
         [_ (zgal-error 'malformed stx "expected (array type length), found ~a" (show stx))])]
      [else (zgal-error 'malformed stx "expected a type, found ~a" (show stx))]))

  ;; A list of (name type) fields; check-type returns a problem with a type or #f
  (define (parse-fields stxs owner check-type)
    (define names (make-namespace (format "in ~a" owner)))
    (for/list ([stx (in-list stxs)])
      (match (syntax->list stx)
        [(list name-stx type-stx)
         (define name (parse-name name-stx "a field name"))
         (names name name-stx)
         (define t (parse-type type-stx))
         (define problem (check-type t))
         (when problem
           (zgal-error 'type-not-allowed type-stx "~a in ~a: ~a" name owner problem))
         (field name t (stx->loc stx))]
        [_ (zgal-error 'malformed stx "expected a field (name type), found ~a" (show stx))])))

  (define (parse-struct stx items)
    (match items
      [(list* _ name-stx fields)
       (define name (parse-called-name name-stx "the struct's name"))
       (top-level name name-stx)
       (when (null? fields)
         (zgal-error 'malformed stx "struct ~a has no fields" name))
       (define decl (struct-decl name (parse-fields fields name std430-problem) (stx->loc stx)))
       (hash-set! structs name decl)
       decl]
      [_ (zgal-error 'malformed stx "expected (struct Name (field type) ...)")]))

  (define (parse-vertex-format stx items)
    (match items
      [(list* _ name-stx attributes)
       (define name (parse-name name-stx "the vertex format's name"))
       (top-level name name-stx)
       (when (null? attributes)
         (zgal-error 'malformed stx "vertex format ~a has no attributes" name))
       (define names (make-namespace (format "in ~a" name)))
       (define decl (vertex-format name (for/list ([a (in-list attributes)]) (parse-attribute a name names)) (stx->loc stx)))
       (hash-set! formats name decl)
       decl]
      [_ (zgal-error 'malformed stx "expected (vertex-format Name (attribute type [#:as storage] [#:default value]) ...)")]))

  ;; (name type [#:as storage])
  (define (parse-attribute stx format-name names)
    (match (expect-list stx "an attribute (name type [#:as storage])")
      [(list* name-stx type-stx option-stxs)
       (define name (parse-name name-stx "an attribute name"))
       (names name name-stx)
       (define t (parse-type type-stx))
       (define problem (interface-problem t))
       (when problem
         (zgal-error 'type-not-allowed type-stx "~a in ~a: ~a" name format-name problem))
       (define-values (positional options) (split-options option-stxs (format "attribute ~a" name)))
       (unless (null? positional)
         (zgal-error 'malformed (car positional) "expected an attribute (name type [#:as storage]), found ~a" (show stx)))
       (check-known-options options '(#:as #:default) (format "attribute ~a" name))
       (define kind (if (vector-t? t) (vector-t-kind t) (scalar-t-kind t)))
       (define default (let ([d (hash-ref options '#:default #f)]) (and d (parse-attribute-default d t name))))
       (define storage
         (choice-option options '#:as all-attribute-storages (format "attribute ~a" name)))
       (when (and storage (not (memq storage (hash-ref attribute-storages kind))))
         (zgal-error 'type-not-allowed (hash-ref options '#:as)
                     "~a in ~a is read as ~a, so it's stored as ~a, not ~a"
                     name format-name (type-name t)
                     (string-join (map symbol->string (hash-ref attribute-storages kind)) ", ") storage))
       (attribute name t (stx->loc stx) storage default)]
      [_ (zgal-error 'malformed stx "expected an attribute (name type [#:as storage] [#:default value]), found ~a" (show stx))]))

  ;; A literal of the attribute's type t: a number, or (vecN number ...)
  (define (parse-attribute-default stx t name)
    (define (number-of s)
      (define n (syntax-e s))
      (define kind (if (vector-t? t) (vector-t-kind t) (scalar-t-kind t)))
      (unless (and (real? n) (or (eq? kind 'float) (exact-integer? n)) (or (not (eq? kind 'uint)) (>= n 0)))
        (zgal-error 'type-mismatch s "attribute ~a's #:default is ~a, so its components are ~a numbers, not ~a"
                    name (type-name t) kind (show s)))
      n)
    (cond
      [(scalar-t? t) (number-of stx)]
      [else
       (define items (syntax->list stx))
       (unless (and items (equal? (head-symbol stx) (string->symbol (type-name t))) (= (length (cdr items)) (vector-t-size t)))
         (zgal-error 'type-mismatch stx "attribute ~a's #:default is a ~a, e.g. (~a ~a), not ~a" name (type-name t) (type-name t)
                     (string-join (make-list (vector-t-size t) "0") " ") (show stx)))
       (map number-of (cdr items))]))

  ;; Parsing another variant of a material already declared (a kernel's without subgroups), whose
  ;; names (its own and its shaders') aren't declared again
  (define parsing-variant? (make-parameter #f))
  (define (parse-material stx items kind)
    (define variant? (parsing-variant?))
    (define form-name (if (eq? kind 'graphics) "graphics-material" "compute-material"))
    (match items
      [(list* _ name-stx all-clause-stxs)
       (define name (parse-name name-stx "the material's name"))
       (unless variant? (top-level name name-stx))
       ;; Its functions, (define ...) clauses, can repeat
       (define-values (define-stxs clause-stxs) (partition (λ (c) (eq? (head-symbol c) 'define)) all-clause-stxs))
       (define clauses (parse-clauses clause-stxs kind form-name))
       (define (clause key) (hash-ref clauses key #f))
       (define (require-clause key)
         (or (clause key)
             (zgal-error 'missing-clause stx "~a ~a has no (~a ...) clause~a" form-name name key
                         (case key
                            [(outputs) "; write (outputs) if it writes no colour attachments"]
                            [else ""]))))
       ;; Names in the shaders' global scope
       (define scope (make-namespace (format "in material ~a" name)))
       (define (declare! name stx)
         (when (hash-ref structs name #f)
           (zgal-error 'duplicate-name stx "~a is already declared as a struct" name))
         (scope name stx))

       (define glsl-shaders (if (clause 'shaders) (parse-shaders (clause 'shaders) name) '()))
       (define workgroup (and (eq? kind 'compute) (parse-workgroup (require-clause 'workgroup))))
       (define input (and (clause 'vertex-input) (parse-vertex-input (clause 'vertex-input) declare!)))
       (define bindings (if (clause 'bindings) (parse-bindings (clause 'bindings) kind declare!) '()))
       (define push
         (if (clause 'push)
             (let ([fields (parse-fields (cdr (syntax->list (clause 'push))) (format "the push constants of ~a" name)
                                         push-problem)])
               (for ([f (in-list fields)] [s (in-list (cdr (syntax->list (clause 'push))))])
                 (declare! (field-name f) s))
               (check-push-size (clause 'push) bindings fields)
               fields)
             '()))
       (define varyings (if (clause 'varyings) (parse-varyings (clause 'varyings) declare!) '()))
       (define shared
         (if (clause 'shared)
             (let ([fields (parse-fields (cdr (syntax->list (clause 'shared))) (format "the shared memory of ~a" name)
                                         (λ (t) #f))])
               (for ([f (in-list fields)] [s (in-list (cdr (syntax->list (clause 'shared))))])
                 (declare! (field-name f) s))
               fields)
             '()))
       (define outputs
         (if (eq? kind 'graphics)
             (let* ([c (require-clause 'outputs)]
                    [fields (parse-fields (cdr (syntax->list c)) (format "the outputs of ~a" name) interface-problem)])
               (for ([f (in-list fields)] [s (in-list (cdr (syntax->list c)))])
                 (declare! (field-name f) s))
               fields)
             '()))
       ;; Its functions, which see its bindings and push constants, then its shaders, which see them
       (define scope-functions (hash-copy functions))
       (for ([d (in-list define-stxs)])
         (match (syntax->list d)
           [(list* _ signature-stx rest) #:when (and (syntax->list signature-stx) (pair? (syntax->list signature-stx)))
            (define f-name-stx (car (syntax->list signature-stx)))
            (define f-name (parse-called-name f-name-stx "the function's name"))
            (when (hash-ref functions f-name #f)
              (zgal-error 'duplicate-name f-name-stx "~a is already a function of the file; material ~a's functions need their own names" f-name name))
            (declare! f-name f-name-stx)
            (define decl (check-function d signature-stx rest f-name-stx #:scope scope-functions
                                         #:globals (material-globals bindings push) #:material name))
            (hash-set! scope-functions f-name decl)]
           [_ (zgal-error 'malformed d "a material's (define ...) is a function, (define (name [parameter : type] ...) : result-type body ...), which its shaders call")]))
       (define zgal-shaders
         (for/list ([stage (in-list (if (eq? kind 'graphics) '(vertex fragment) '(compute)))]
                    #:when (clause stage))
           (parse-stage-body (clause stage) stage name bindings push input varyings outputs shared scope-functions)))
       (define shaders (append glsl-shaders zgal-shaders))
       (check-stages stx kind name shaders)
       (define m (material kind name shaders (infer-accesses bindings shaders kind) push input varyings outputs
                           workgroup shared (stx->loc stx)))
       (unless variant? (hash-set! materials name m))
       m]
      [_ (zgal-error 'malformed stx "expected (~a Name clause ...)" form-name)]))

  (define (parse-clauses stxs kind form-name)
    (define allowed
      (if (eq? kind 'graphics)
          '(shaders vertex fragment vertex-input bindings push varyings outputs)
          '(shaders compute workgroup bindings push shared)))
    (for/fold ([clauses (hasheq)]) ([stx (in-list stxs)])
      (define key (head-symbol stx))
      (unless key
        (zgal-error 'malformed stx "expected a clause (name ...), found ~a" (show stx)))
      (unless (memq key allowed)
        (zgal-error 'unknown-clause stx "~a has no ~a clause; it takes ~a" form-name key
                    (string-join (map symbol->string allowed) ", ")))
      (when (hash-ref clauses key #f)
        (zgal-error 'duplicate-clause stx "~a has two (~a ...) clauses" form-name key))
      (hash-set clauses key stx)))

  ;; A shader's name, which the shader library finds it by: unique in the file
  (define (claim-shader-name! name material-name where)
    (match (and (not (parsing-variant?)) (hash-ref shader-names name #f))
      [(cons other previous)
       (zgal-error 'shader-reused where "~a is already a shader of material ~a (line ~a)"
                   name other (loc-line previous))]
      [_ (hash-set! shader-names name (cons material-name (stx->loc where)))]))

  ;; A material has one shader per stage: GLSL bodies (shaders) or ZGAL ones (vertex, fragment,
  ;; compute clauses)
  (define (check-stages stx kind material-name shaders)
    (define stages (map shader-stage shaders))
    (define expected (if (eq? kind 'graphics) '(vertex fragment) '(compute)))
    (unless (and (= (length stages) (length expected))
                 (for/and ([s (in-list expected)]) (memq s stages)))
      (zgal-error 'bad-shader stx "~a needs ~a, but has ~a" material-name
                  (if (eq? kind 'graphics)
                      "a vertex and a fragment shader (a .vert and a .frag in shaders, or vertex and fragment clauses)"
                      "one compute shader (a .comp in shaders, or a compute clause)")
                  (if (null? shaders) "none" (string-join (map shader-name shaders) " and ")))))

  (define (parse-shaders stx material-name)
    (define shaders
      (for/list ([path-stx (in-list (cdr (syntax->list stx)))])
        (define path (syntax-e path-stx))
        (unless (string? path)
          (zgal-error 'malformed path-stx "expected a shader path (a string), found ~a" (show path-stx)))
        (define stage
          (match (regexp-match #px"\\.(vert|frag|comp)$" path)
            [(list _ "vert") 'vertex]
            [(list _ "frag") 'fragment]
            [(list _ "comp") 'compute]
            [_ (zgal-error 'bad-shader path-stx
                           "~a doesn't end in .vert, .frag or .comp, which give a shader's stage" path)]))
        (define resolved (path->string (simplify-path (build-path directory path) #f)))
        (define name (path->string (file-name-from-path resolved)))
        (claim-shader-name! name material-name path-stx)
        (shader resolved name stage (stx->loc path-stx))))
    shaders)

  ;; ---------------------------------------------------------------------------------------------
  ;; ZGAL bodies

  (define functions (make-hash))     ; name -> function-decl
  (define constants (make-hash))     ; name -> constant-decl
  (define summaries (make-hash))     ; function name -> its opaque parameters' accesses
  (define cpu-functions (mutable-set))
  (define cpu-constants (mutable-set))

  (define (parse-value-type stx #:void? [void? #f])
    (case (syntax-e stx)
      [(texture2D sampler image2D) (opaque-t (syntax-e stx))]
      [(void) (if void? (void-t) (zgal-error 'unknown-type stx "void is only a function's result type"))]
      [else (parse-type stx)]))

  ;; The file's functions and constants used by checked code, closed over what they use, in
  ;; declaration order
  (define declaration-order (make-hash))
  (define (uses-closure direct)
    (define seen (mutable-set))
    (let visit ([names (set->list direct)])
      (for ([n (in-list names)] #:unless (set-member? seen n))
        (set-add! seen n)
        (visit (match (or (hash-ref functions n #f) (hash-ref constants n #f))
                 [(function-decl _ _ _ _ _ uses _) uses]
                 [(constant-decl _ _ _ _ uses _) uses]))))
    (sort (set->list seen) < #:key (λ (n) (hash-ref declaration-order n))))

  ;; A function's declaration, checked: (define (name [parameter : type] ...) : result body ...).
  ;; A material's functions (material: its name) see its bindings and push constants (globals) and
  ;; earlier functions of it (scope: the functions bodies see), and are inlined where they're called.
  (define (check-function stx signature-stx rest name-stx #:scope [scope functions] #:globals [globals (hash)] #:material [material #f])
       (define param-stxs (cdr (syntax->list signature-stx)))
       (define name (parse-called-name name-stx "the function's name"))
       (define-values (result-type body-stxs)
         (match rest
           [(list* colon result-stx body) #:when (and (eq? (syntax-e colon) ':) (pair? body))
            (values (parse-value-type result-stx #:void? #t) body)]
           [_ (zgal-error 'malformed stx "expected (define (~a [parameter : type] ...) : result-type body ...)" name)]))
       (define ctx (make-context structs scope constants globals 'function))
       (define param-names (make-namespace (format "as a parameter of ~a" name)))
       (define-values (params locals)
         (for/fold ([params '()] [locals (hash)] #:result (values (reverse params) locals))
                   ([p (in-list param-stxs)])
           (match (syntax->list p)
             [(list n colon t) #:when (eq? (syntax-e colon) ':)
              (define param (local-name n ctx))
              (param-names param n)
              (define type (parse-value-type t))
              (define spelling (spell! ctx param))
              (values (cons (cons spelling type) params) (hash-set locals param (t-local spelling type (stx->loc n))))]
             [_ (zgal-error 'malformed p "expected a parameter [name : type]")])))
       (define body
         (let ([b (check-sequence body-stxs stx ctx locals)])
           (if (void-t? result-type)
               (begin
                 (unless (void-t? (texpr-type b))
                   (zgal-error 'type-mismatch stx "~a returns void, but its body is ~a" name (a-type (texpr-type b))))
                 b)
               (coerce (settle-unless-literal b result-type) result-type stx (format "~a's body" name)))))
       (define uses (uses-closure (context-uses ctx)))
       (cond
         [material (material-function-decl name params result-type body #f uses (stx->loc stx) material)]
         [else
          (define cpu? (and (not (void-t? result-type))
                            (not (ormap (λ (p) (opaque-t? (cdr p))) params))
                            (cpu-expression? body cpu-functions cpu-constants)))
          (when cpu? (set-add! cpu-functions name))
          (hash-set! summaries name (parameter-accesses params body summaries))
          (function-decl name params result-type body cpu? uses (stx->loc stx))]))

  (define (parse-define stx items)
    (match items
      ;; A function
      [(list* _ signature-stx rest) #:when (and (syntax->list signature-stx) (pair? (syntax->list signature-stx)))
       (define name-stx (car (syntax->list signature-stx)))
       (top-level (parse-called-name name-stx "the function's name") name-stx)
       (define decl (check-function stx signature-stx rest name-stx))
       (hash-set! declaration-order (function-decl-name decl) (hash-count declaration-order))
       (hash-set! functions (function-decl-name decl) decl)
       decl]
      ;; A constant
      [(list _ name-stx value-stx) #:when (symbol? (syntax-e name-stx))
       (define name (parse-name name-stx "the constant's name"))
       (top-level name name-stx)
       (define ctx (make-context structs functions constants (hash) 'function))
       (define value (settle (check value-stx ctx (hash))))
       (unless (constant-expression? value)
         (zgal-error 'malformed value-stx "a constant is computed from literals, other constants, constructors, operators and built-in functions"))
       (define uses (uses-closure (context-uses ctx)))
       (define cpu? (cpu-expression? value cpu-functions cpu-constants))
       (when cpu? (set-add! cpu-constants name))
       (define decl (constant-decl name (texpr-type value) value cpu? uses (stx->loc stx)))
       (hash-set! declaration-order name (hash-count declaration-order))
       (hash-set! constants name decl)
       decl]
      [_ (zgal-error 'malformed stx "expected (define name value) or (define (name [parameter : type] ...) : type body ...)")]))

  (define (settle-unless-literal b t) b)

  ;; (extern-glsl "file.glsl" ((name [parameter : type] ...) : result) ...): functions written in
  ;; GLSL, for what ZGAL bodies can't express yet. They take and give values (no textures, images
  ;; or effects), so they can't touch bindings and inference stays sound. The path is relative to
  ;; the file declaring them.
  (define (parse-extern-glsl stx items)
    (match items
      [(list* _ path-stx signatures) #:when (and (string? (syntax-e path-stx)) (pair? signatures))
       (define declared-in
         (let-values ([(dir _f _d) (split-path (loc-source (stx->loc stx)))]) (if (path? dir) dir (string->path "."))))
       (define path (path->string (simplify-path (build-path declared-in (syntax-e path-stx)) #f)))
       (for ([sig (in-list signatures)])
         (match (syntax->list sig)
           [(list signature colon result) #:when (and (syntax->list signature) (eq? (syntax-e colon) ':))
            (match-define (cons name-stx param-stxs) (syntax->list signature))
            (define name (parse-called-name name-stx "the function's name"))
            (top-level name name-stx)
            (define param-names (make-namespace (format "as a parameter of ~a" name)))
            (define params
              (for/list ([p (in-list param-stxs)])
                (match (syntax->list p)
                  [(list n colon2 t) #:when (eq? (syntax-e colon2) ':)
                   (define p-name (parse-name n "a parameter's name"))
                   (param-names p-name n)
                   (cons p-name (parse-type t))]
                  [_ (zgal-error 'malformed p "expected a parameter [name : type]")])))
            (define decl (glsl-function-decl name params (parse-type result) #f #f '() (stx->loc sig) path))
            (hash-set! summaries name (for/list ([_ (in-list params)]) (set)))
            (hash-set! declaration-order name (hash-count declaration-order))
            (hash-set! functions name decl)]
           [_ (zgal-error 'malformed sig "expected a GLSL function's signature ((name [parameter : type] ...) : result-type)")]))]
      [_ (zgal-error 'malformed stx "expected (extern-glsl \"file.glsl\" ((name [parameter : type] ...) : result-type) ...)")]))

  (define (constant-expression? e)
    (match e
      [(or (t-lit _ _ _) (t-global 'constant _ _ _ _)) #t]
      [(or (t-op _ args _ _) (t-construct args _ _) (t-call _ args _ _)) (andmap constant-expression? args)]
      [_ #f]))

  ;; The names of a material's bindings and push constants, which its functions and shaders see
  (define (material-globals bindings push)
    (define (binding-type b)
      (case (binding-kind b)
        [(texture) (opaque-t 'texture2D)]
        [(sampler) (opaque-t 'sampler)]
        [(storage-image) (opaque-t 'image2D)]
        [(buffer) (binding-element b)]
        [(buffer-array) (runtime-array-t (binding-element b))]
        [(array) (binding-array-t (binding-type (binding-element b)))]))
    (for/fold ([globals (hash)]) ([g (in-list (append (for/list ([b (in-list bindings)])
                                                         (global 'binding (binding-name b) (binding-name b) (binding-type b) b))
                                                       (for/list ([f (in-list push)])
                                                         (global 'push (field-name f) (field-name f) (field-type f) f))))])
      (hash-set globals (global-name g) g)))

  ;; A stage's body: checked with the material's names in scope
  (define (parse-stage-body stx stage material-name bindings push input varyings outputs shared scope-functions)
    (define body-stxs (cdr (syntax->list stx)))
    (define globals (make-hash))
    (define (global! name g where)
      (when (hash-ref globals name #f)
        (zgal-error 'duplicate-name where "~a names two things in material ~a's ~a shader" name material-name stage))
      (hash-set! globals name g))
    (for ([(n g) (in-hash (material-globals bindings push))])
      (global! n g stx))
    (when (and (eq? stage 'vertex) input)
      (for ([entry (in-list (vertex-input-attributes input))])
        (define a (cdr entry))
        (global! (field-name a) (global 'input (field-name a) (input-variable-name (field-name a)) (field-type a) a) stx)))
    (when (eq? stage 'fragment)
      (for ([v (in-list varyings)])
        (global! (varying-name v) (global 'varying (varying-name v) (varying-name v) (varying-type v) v) stx))
      (for ([o (in-list outputs)])
        (global! (field-name o) (global 'output (field-name o) (field-name o) (field-type o) o) stx)))
    (for ([f (in-list shared)])
      (global! (field-name f) (global 'shared (field-name f) (field-name f) (field-type f) f) stx))
    (when (eq? stage 'vertex)
      (for ([v (in-list varyings)])
        (global! (varying-name v) (global 'output (varying-name v) (varying-name v) (varying-type v) v) stx)))
    (define ctx (make-context structs scope-functions constants (for/hash ([(k v) (in-hash globals)]) (values k v)) stage))
    ;; What a stage's results must name
    (define expected
      (case stage
        [(vertex) (cons (cons "position" (vector-t 'float 4)) (for/list ([v (in-list varyings)]) (cons (varying-name v) (varying-type v))))]
        [(fragment) (for/list ([o (in-list outputs)]) (cons (field-name o) (field-type o)))]
        [else '()]))
    (define (results-checker fields where)
      (unless (memq stage '(vertex fragment))
        (zgal-error 'bad-results where "a compute shader has no results: it writes buffers and images"))
      (define given (make-hash))
      (for ([f (in-list fields)])
        (match-define (list name e k) f)
        (define t (or (for/first ([x (in-list expected)] #:when (string=? (car x) name)) (cdr x))
                      (zgal-error 'bad-results k "the ~a shader has no output ~a; it gives ~a" stage name
                                  (string-join (map car expected) ", "))))
        (when (hash-ref given name #f)
          (zgal-error 'bad-results k "~a is given twice" name))
        (hash-set! given name (coerce e t k (format "output ~a" name))))
      (t-results (for/list ([x (in-list expected)])
                   (cons (car x) (or (hash-ref given (car x) #f)
                                     (zgal-error 'bad-results where "the ~a shader's results don't give ~a" stage (car x)))))
                 (stx->loc where)))
    (define body
      (parameterize ([current-results-checker results-checker])
        (cond
          [(null? body-stxs)
           (unless (null? expected)
             (zgal-error 'bad-results stx "the ~a shader's body is empty, but it gives ~a" stage (string-join (map car expected) ", ")))
           (t-results '() (stx->loc stx))]
          [else
           (define b (check-sequence body-stxs stx ctx (hash)))
           (match* (stage (texpr-type b))
             [(_ (results-t)) b]
             ;; A fragment shader's one output can be its body's value
             [('fragment t) #:when (and (= (length expected) 1) (not (void-t? t)))
              (t-results (list (cons (car (first expected)) (coerce b (cdr (first expected)) stx (format "output ~a" (car (first expected))))))
                         (stx->loc stx))]
             [('compute (void-t)) b]
             ;; Ending with an atomic, whose previous value it ignores
             [('compute _) #:when (discardable? b) b]
             [(_ (void-t)) #:when (null? expected) (t-begin (list b (t-results '() (stx->loc stx))) (results-t) (stx->loc stx))]
             [(_ t) (zgal-error 'bad-results stx "the ~a shader's body is ~a, but should give ~a" stage (a-type t)
                                (case stage
                                  [(compute) "nothing (it writes buffers and images)"]
                                  [else (format "(results ~a)" (string-join (map (λ (x) (format "#:~a ..." (car x))) expected) " "))]))])])))
    (when (eq? stage 'compute) (check-barriers body summaries))
    (define name (format "~a.~a" material-name (case stage [(vertex) "vert"] [(fragment) "frag"] [(compute) "comp"])))
    (claim-shader-name! name material-name stx)
    (zgal-shader #f name stage (stx->loc stx) body (uses-closure (context-uses ctx))))

  ;; Fills in the accesses and stages of bindings from ZGAL bodies, and checks declared ones
  (define (infer-accesses bindings shaders kind)
    (define all-zgal? (andmap zgal-shader? shaders))
    (define accesses (make-hash))
    (define stages-using (make-hash))
    (for ([s (in-list shaders)] #:when (zgal-shader? s))
      (for ([(name access) (in-hash (binding-accesses (zgal-shader-body s) summaries))])
        (hash-update! accesses name (λ (a) (set-union a access)) (set))
        (hash-update! stages-using name (λ (a) (set-add a (shader-stage s))) (set))))
    (for/list ([b (in-list bindings)])
      (define storage? (memq (binding-resource-kind b) '(storage-image buffer buffer-array)))
      (define used (hash-ref accesses (binding-name b) (set)))
      (define inferred
        (cond [(set-empty? used) #f]
              [(and (set-member? used 'read) (set-member? used 'write)) 'read-write]
              [(set-member? used 'write) 'write]
              ;; Read, or only sized
              [else 'read]))
      (define access
        (cond
          [(not storage?) #f]
          [(not all-zgal?)
           (or (binding-access b)
               (zgal-error 'bad-binding (binding-loc b)
                           "binding ~a has no access, which can only be inferred when all of its material's shaders are ZGAL bodies; declare read, write or read-write"
                           (binding-name b)))]
          [(not inferred)
           (or (binding-access b)
               (zgal-error 'unused-binding (binding-loc b) "binding ~a isn't used by the material's shaders, so its access can't be inferred"
                           (binding-name b)))]
          [(and (binding-access b) (not (eq? (binding-access b) inferred)))
           (zgal-error 'access-mismatch (binding-loc b) "binding ~a is declared ~a, but the shaders ~a it"
                       (binding-name b) (binding-access b)
                       (case inferred [(read) "only read"] [(write) "only write"] [else "read and write"]))]
          [else inferred]))
      (define stages
        (cond
          [(or (eq? kind 'compute) (eq? (binding-kind b) 'sampler)) (binding-stages b)]
          [(not all-zgal?) (binding-stages b)]
          [else
           (define using (filter (λ (s) (set-member? (hash-ref stages-using (binding-name b) (set)) s)) '(vertex fragment)))
           (cond
             [(null? (binding-stages b)) using]
             [(and (pair? using) (not (equal? (sort (binding-stages b) symbol<?) (sort using symbol<?))))
              (zgal-error 'access-mismatch (binding-loc b) "binding ~a is declared for the ~a shader~a, but the ~a shader~a use~a it"
                          (binding-name b) (string-join (map symbol->string (binding-stages b)) " and ")
                          (if (= (length (binding-stages b)) 1) "" "s")
                          (string-join (map symbol->string using) " and ") (if (= (length using) 1) "" "s")
                          (if (= (length using) 1) "s" ""))]
             [else (binding-stages b)])]))
      (struct-copy binding b [access access] [stages stages])))

  (define (parse-workgroup stx)
    (define sizes (cdr (syntax->list stx)))
    (unless (and (<= 1 (length sizes) 3)
                 (for/and ([s (in-list sizes)]) (exact-positive-integer? (syntax-e s))))
      (zgal-error 'bad-workgroup stx "expected (workgroup x [y [z]]) with positive integers, found ~a" (show stx)))
    (define given (map syntax-e sizes))
    (append given (make-list (- 3 (length given)) 1)))

  (define (parse-vertex-input stx declare!)
    (match (syntax->list stx)
      [(list* _ format-stx attribute-stxs)
       (define format-name (syntax-e format-stx))
       (define fmt
         (or (and (symbol? format-name) (hash-ref formats (symbol->string format-name) #f))
             (zgal-error 'unknown-name format-stx "~a isn't a vertex format declared earlier in the file"
                         (show format-stx))))
       (define all (for/list ([a (in-list (vertex-format-attributes fmt))] [i (in-naturals)]) (cons i a)))
       (define chosen
         (if (null? attribute-stxs)
             all
             (let ([names (make-namespace "in this vertex-input")])
               (for/list ([a-stx (in-list attribute-stxs)])
                 (define a-name (syntax-e a-stx))
                 (names (format "~a" a-name) a-stx)
                 (or (for/first ([entry (in-list all)]
                                 #:when (and (symbol? a-name) (string=? (field-name (cdr entry)) (symbol->string a-name))))
                       entry)
                     (zgal-error 'unknown-name a-stx "vertex format ~a has no attribute ~a"
                                 (vertex-format-name fmt) (show a-stx)))))))
       (for ([entry (in-list chosen)])
         (declare! (input-variable-name (field-name (cdr entry))) stx))
       (vertex-input fmt chosen (stx->loc stx))]
      [_ (zgal-error 'malformed stx "expected (vertex-input Format [attribute ...])")]))

  (define (parse-bindings stx kind declare!)
    (for/list ([b-stx (in-list (cdr (syntax->list stx)))])
      (match (syntax->list b-stx)
        [(list* name-stx kind-stx options)
         (define name (parse-name name-stx "a binding name"))
         (declare! name name-stx)
         (define-values (binding-kind element access) (parse-binding-kind kind-stx name))
         (define stages (parse-binding-options options b-stx kind binding-kind name))
         (binding name binding-kind element access stages (stx->loc b-stx))]
        [_ (zgal-error 'malformed b-stx "expected a binding (name kind [#:stages (stage ...)]), found ~a"
                       (show b-stx))])))

  (define (parse-access stx kind-name)
    (define a (syntax-e stx))
    (unless (memq a '(read write read-write))
      (zgal-error 'bad-binding stx "a ~a's access is read, write or read-write, not ~a" kind-name (show stx)))
    a)

  (define (parse-binding-kind stx name)
    (define head (or (head-symbol stx) (syntax-e stx)))
    (define items (syntax->list stx))
    (match head
      ;; An array of entries of another kind; its access is the array's
      ['array
       (match items
         [(list _ element-stx)
          (define-values (kind element access) (parse-binding-kind element-stx name))
          (when (memq kind '(sampler array))
            (zgal-error 'bad-binding element-stx "binding ~a: an array holds textures, storage images or buffers, not ~a"
                        name (if (eq? kind 'sampler) "samplers" "arrays")))
          (values 'array (binding name kind element #f '() (stx->loc element-stx)) access)]
         [_ (zgal-error 'bad-binding stx "binding ~a: expected (array kind), e.g. (array texture) or (array (buffer-array float))" name)])]
      ['texture #:when (symbol? (syntax-e stx)) (values 'texture #f #f)]
      ['sampler #:when (symbol? (syntax-e stx)) (values 'sampler #f #f)]
      ['storage-image
       (match items
         [(list _ access) (values 'storage-image #f (parse-access access "storage image"))]
         ;; Inferred from ZGAL bodies
         [(list _) (values 'storage-image #f #f)]
         [_ (zgal-error 'bad-binding stx "binding ~a: expected (storage-image [access]), with access read, write or read-write" name)])]
      [(or 'buffer 'buffer-array)
       (match items
         [(list _ type-stx access ...)
          #:when (<= (length access) 1)
          (define t (parse-type type-stx))
          (when (array-t? t)
            (zgal-error 'type-not-allowed type-stx
                        "binding ~a: a ~a holds a struct, scalar, vector or matrix; for an array of them use (buffer-array type access)"
                        name head))
          ;; A buffer-array's elements are laid out like an array's
          (define problem (std430-problem (if (eq? head 'buffer-array) (array-t t 1) t)))
          (when problem (zgal-error 'type-not-allowed type-stx "binding ~a: ~a" name problem))
          (values head t (and (pair? access) (parse-access (first access) (symbol->string head))))]
         [_ (zgal-error 'bad-binding stx "binding ~a: expected (~a type [access]), with access read, write or read-write"
                        name head)])]
      [_ (zgal-error 'bad-binding stx
                     "binding ~a: unknown kind ~a; expected texture, sampler, (storage-image access), (buffer type access), (buffer-array type access) or (array kind)"
                     name (show stx))]))

  (define (parse-binding-options options stx material-kind binding-kind name)
    (match options
      ['() '()]
      [(list keyword-stx stages-stx) #:when (eq? (syntax-e keyword-stx) '#:stages)
       (when (eq? material-kind 'compute)
         (zgal-error 'bad-stages keyword-stx "binding ~a: compute materials' bindings are used by the compute shader, so they take no #:stages" name))
       (when (eq? binding-kind 'sampler)
         (zgal-error 'bad-stages keyword-stx "binding ~a: samplers aren't synchronized, so they take no #:stages" name))
       (define stage-stxs (syntax->list stages-stx))
       (unless (and stage-stxs (pair? stage-stxs))
         (zgal-error 'bad-stages stages-stx "binding ~a: expected a list of stages, e.g. (vertex fragment)" name))
       (define names (make-namespace "in these stages"))
       (for/list ([s (in-list stage-stxs)])
         (define stage (syntax-e s))
         (unless (memq stage '(vertex fragment))
           (zgal-error 'bad-stages s "binding ~a: ~a isn't a stage of a graphics material (vertex, fragment)" name (show s)))
         (names (symbol->string stage) s)
         stage)]
      [_ (zgal-error 'malformed stx "expected a binding (name kind [#:stages (stage ...)]), found ~a" (show stx))]))

  (define (check-push-size stx bindings fields)
    (define start (* 4 (length bindings)))
    (define-values (_ end) (place-fields fields start))
    (when (> end push-constant-limit)
      (zgal-error 'push-too-large stx
                  "the push constants take ~a bytes (~a for ~a binding indices, then the push fields), more than the ~a the pipeline layout has"
                  end start (length bindings) push-constant-limit)))

  (define (parse-varyings stx declare!)
    (for/list ([v-stx (in-list (cdr (syntax->list stx)))])
      (define-values (name-stx type-stx interpolation-stx)
        (match (syntax->list v-stx)
          [(list n t) (values n t #f)]
          [(list n t i) (values n t i)]
          [_ (zgal-error 'malformed v-stx "expected a varying (name type [flat | noperspective]), found ~a" (show v-stx))]))
      (define name (parse-name name-stx "a varying name"))
      (declare! name name-stx)
      (define t (parse-type type-stx))
      (define problem (interface-problem t))
      (when problem (zgal-error 'type-not-allowed type-stx "varying ~a: ~a" name problem))
      (define interpolation
        (if interpolation-stx
            (let ([i (syntax-e interpolation-stx)])
              (unless (memq i '(flat noperspective smooth))
                (zgal-error 'malformed interpolation-stx "expected flat, noperspective or smooth, found ~a" (show interpolation-stx)))
              i)
            'smooth))
      (when (and (memq (if (vector-t? t) (vector-t-kind t) (scalar-t-kind t)) '(int uint))
                 (not (eq? interpolation 'flat)))
        (zgal-error 'varying-not-flat v-stx "varying ~a is a ~a, so it must be flat: (~a ~a flat)"
                    name (type-name t) name (type-name t)))
      (varying name t interpolation (stx->loc v-stx))))

;; ---------------------------------------------------------------------------------------------
  ;; Library kernels: materials in dsl/lib/kernels, instantiated for the graphs using them

  (define kernel-instances (make-hash))   ; (list file suffix) -> hash of template name -> material
  (define kernel-materials '())           ; instantiated, in order
  (define kernel-files '())               ; the templates read, as locations name them
  (define kernel-fallbacks (make-hash))   ; material name -> its variant without subgroups
  ;; The materials of the template `file` (in dsl/lib/kernels) with symbols replaced by
  ;; substitutions (a hash of symbol -> datum), (%if flag then [else]) and (%unless flag then)
  ;; chosen by flags (and the flag
  ;; subgroups), and each material's name followed by suffix. Returns a hash from the template's
  ;; material names to the instances; instantiating the same file and suffix again gives the same
  ;; materials. Materials choosing by subgroups get a variant without them in kernel-fallbacks.
  (define (instantiate-kernel file substitutions flags suffix)
    (hash-ref! kernel-instances (list file suffix)
               (λ ()
                 (define complete (simplify-path (build-path library-directory "kernels" file)))
                 (define shown (display-path complete))
                 (unless (member shown kernel-files) (set! kernel-files (append kernel-files (list shown))))
                 (define forms (read-forms shown (open-input-string (file->string complete))))
                 (define names
                   (for/hash ([f (in-list forms)] #:when (eq? (head-symbol f) 'compute-material))
                     (define n (syntax-e (cadr (syntax->list f))))
                     (values n (string->symbol (format "~a~a" n suffix)))))
                 (parameterize ([library-names-allowed? #t])
                   (for/hash ([f (in-list forms)])
                     (define (instance flags) (substitute-template f (hash-union* substitutions names) flags))
                     (define with (instance (cons 'subgroups flags)))
                     (define without (instance flags))
                     (define m (parse-material with (syntax->list with) 'compute))
                     (define fallback
                       (and (not (equal? (syntax->datum with) (syntax->datum without)))
                            (parameterize ([parsing-variant? #t]) (parse-material without (syntax->list without) 'compute))))
                     (when fallback (hash-set! kernel-fallbacks (material-name m) fallback))
                     (set! kernel-materials (append kernel-materials (list m)))
                     (values (symbol->string (syntax-e (cadr (syntax->list f)))) m))))))

  (define graphs (make-hash))   ; name -> graph
  (define-values (all-forms imports) (expand-imports forms source))
  (define main-forms '())   ; (main ...) forms, parsed once the apps are
  (define-values (structs-out formats-out materials-out graphs-out apps-out)
    (for/fold ([ss '()] [fs '()] [ms '()] [gs '()] [as '()]
               #:result (values (reverse ss) (reverse fs) (reverse ms) (reverse gs) (reverse as)))
              ([form (in-list all-forms)])
      (define items (syntax->list form))
      (match (head-symbol form)
        ['struct (values (cons (parse-struct form items) ss) fs ms gs as)]
        ['vertex-format (values ss (cons (parse-vertex-format form items) fs) ms gs as)]
        ['graphics-material (values ss fs (cons (parse-material form items 'graphics) ms) gs as)]
        ['compute-material (values ss fs (cons (parse-material form items 'compute) ms) gs as)]
        ['graph
         (define g (parse-graph form parse-type top-level materials formats instantiate-kernel))
         (hash-set! graphs (graph-name g) g)
         (values ss fs ms (cons g gs) as)]
        ['app
         (values ss fs ms gs (cons (parse-app form parse-type top-level structs functions constants cpu-functions graphs)
                                   as))]
        ['main (set! main-forms (append main-forms (list form))) (values ss fs ms gs as)]
        ['define (parse-define form items) (values ss fs ms gs as)]
        ['extern-glsl (parse-extern-glsl form items) (values ss fs ms gs as)]
        ['import (values ss fs ms gs as)]
        [_ (zgal-error 'unknown-form form
                       "expected import, struct, vertex-format, define, extern-glsl, graphics-material, compute-material, graph, app or main, found ~a"
                       (let ([head (head-symbol form)]) (if head (format "(~a ...)" head) (show form))))])))
  (define main-out
    (match main-forms
      ['() #f]
      [(list f) (parse-main f apps-out)]
      [(list* _ f _) (zgal-error 'duplicate-clause f "a file has one main")]))
  (program source structs-out formats-out (append materials-out kernel-materials) graphs-out
           (sort (append (hash-values functions) (hash-values constants)) <
                 #:key (λ (d) (hash-ref declaration-order (if (function-decl? d) (function-decl-name d) (constant-decl-name d)))))
           apps-out
           main-out
           (append imports kernel-files)
           (for/hash ([(k v) (in-hash kernel-fallbacks)]) (values k v))))

;; ---------------------------------------------------------------------------------------------
;; Imports

;; A path as locations (and #line directives) name it: relative to the working directory, where the
;; compilers reading the generated code run, so no machine's absolute paths end up in them
(define (display-path p)
  (define absolute (simplify-path (path->complete-path p)))
  (path->string (find-relative-path (simplify-path (current-directory)) absolute)))

;; The forms of the file at `source`, with each import replaced by the imported file's forms
;; (before it, as declarations come before their uses), and the imported files' paths. A file is
;; imported once however many files import it.
(define (expand-imports forms source)
  (define imported (make-hash))   ; complete path -> #t
  (define order '())
  (define (expand forms directory chain)
    (append*
     (for/list ([form (in-list forms)])
       (cond
         [(eq? (head-symbol form) 'import)
          (define items (syntax->list form))
          (unless (= (length items) 2)
            (zgal-error 'bad-import form "expected (import module) for the standard library, or (import \"file.zgal\")"))
          (define target (syntax-e (second items)))
          (define file
            (cond
              [(symbol? target)
               (define p (build-path library-directory (format "~a.zgal" target)))
               (unless (file-exists? p)
                 (zgal-error 'bad-import (second items) "the standard library has no module ~a; it has ~a" target
                             (string-join (sort (for/list ([f (in-list (directory-list library-directory))]
                                                           #:when (regexp-match? #px"[.]zgal$" (path->string f)))
                                                  (regexp-replace #px"[.]zgal$" (path->string f) ""))
                                                string<?)
                                          ", ")))
               p]
              [(string? target)
               (define p (if (absolute-path? target) (string->path target) (build-path directory target)))
               (unless (file-exists? p)
                 (zgal-error 'bad-import (second items) "~a doesn't exist" (path->string (simplify-path p #f))))
               p]
              [else (zgal-error 'bad-import (second items) "expected a module name or a file's path, found ~a" (show (second items)))]))
          (define complete (simplify-path (path->complete-path file)))
          (cond
            [(member complete chain)
             (zgal-error 'bad-import form "~a imports itself, through ~a" (display-path complete)
                         (string-join (map display-path (reverse chain)) " -> "))]
            [(hash-ref imported complete #f) '()]
            [else
             (hash-set! imported complete #t)
             (define shown (display-path complete))
             (define imported-forms (read-forms shown (open-input-string (file->string complete))))
             (for ([f (in-list imported-forms)])
               (unless (memq (head-symbol f) '(import struct vertex-format define extern-glsl graphics-material compute-material graph app))
                 (zgal-error 'bad-import f "an imported file declares structs, vertex formats, functions, constants, GLSL functions, materials, graphs, apps and imports, not ~a (a program's main is in its own file)"
                             (let ([h (head-symbol f)]) (if h (format "(~a ...)" h) (show f))))))
             (define expanded
               (expand imported-forms (let-values ([(dir _f _d) (split-path complete)]) dir) (cons complete chain)))
             (set! order (cons shown order))
             expanded])]
         [else (list form)]))))
  (define directory
    (let-values ([(dir _name _must-be-dir) (split-path source)])
      (if (path? dir) dir (current-directory))))
  (define start (simplify-path (path->complete-path source)))
  (values (expand forms directory (list start)) (reverse order)))

;; A kernel template's form with symbols replaced (substitutions: symbol -> datum, given the symbol's
;; location) and (%if flag then [else]) forms chosen by flags (a list of symbols), dropping those
;; without an else whose flag isn't set
(define (substitute-template stx substitutions flags)
  (define dropped (gensym 'dropped))
  (define (walk stx)
    (define d (syntax-e stx))
    (cond
      [(symbol? d)
       (if (hash-has-key? substitutions d) (datum->syntax #f (hash-ref substitutions d) stx) stx)]
      [(syntax->list stx)
       => (λ (items)
            (cond
              [(and (pair? items) (eq? (syntax-e (car items)) '%if))
               (match (cdr items)
                 [(list flag then) (if (memq (syntax-e flag) flags) (walk then) dropped)]
                 [(list flag then else) (walk (if (memq (syntax-e flag) flags) then else))])]
              [(and (pair? items) (eq? (syntax-e (car items)) '%unless))
               (match (cdr items)
                 [(list flag then) (if (memq (syntax-e flag) flags) dropped (walk then))])]
              [else
               (datum->syntax #f (filter (λ (x) (not (eq? x dropped))) (map walk items)) stx)]))]
      [else stx]))
  (walk stx))

;; Vertex inputs are named "in" followed by the attribute, e.g. inPosition
(define (input-variable-name attribute)
  (string-append "in" (string-upcase (substring attribute 0 1)) (substring attribute 1)))
(provide input-variable-name)
