#lang racket/base
;; Reads a .zgal file and checks it, producing the program (ast.rkt). Every problem is reported
;; as a catalogued error (errors.rkt) at the syntax responsible.

(require racket/list
         racket/match
         racket/path
         racket/string
         "ast.rkt"
         "errors.rkt"
         "graph.rkt"
         "layout.rkt"
         "names.rkt"
         "syntax.rkt")

(provide read-forms
         parse-program
         push-constant-limit)

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
         "mat2" (matrix-t 2) "mat3" (matrix-t 3) "mat4" (matrix-t 4))
   (for*/hash ([kind '(float int uint bool)] [n '(2 3 4)])
     (define t (vector-t kind n))
     (values (type-name t) t))))


;; ---------------------------------------------------------------------------------------------
;; The program

;; forms: the syntax read from the file at `source` (its path, as given)
(define (parse-program forms source)
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
       (define name (parse-name name-stx "the struct's name"))
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
       (define decl (vertex-format name (parse-fields attributes name interface-problem) (stx->loc stx)))
       (hash-set! formats name decl)
       decl]
      [_ (zgal-error 'malformed stx "expected (vertex-format Name (attribute type) ...)")]))

  (define (parse-material stx items kind)
    (define form-name (if (eq? kind 'graphics) "graphics-material" "compute-material"))
    (match items
      [(list* _ name-stx clause-stxs)
       (define name (parse-name name-stx "the material's name"))
       (top-level name name-stx)
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

       (define shaders (parse-shaders (require-clause 'shaders) kind name))
       (define workgroup (and (eq? kind 'compute) (parse-workgroup (require-clause 'workgroup))))
       (define input (and (clause 'vertex-input) (parse-vertex-input (clause 'vertex-input) declare!)))
       (define bindings (if (clause 'bindings) (parse-bindings (clause 'bindings) kind declare!) '()))
       (define push
         (if (clause 'push)
             (let ([fields (parse-fields (cdr (syntax->list (clause 'push))) (format "the push constants of ~a" name)
                                         std430-problem)])
               (for ([f (in-list fields)] [s (in-list (cdr (syntax->list (clause 'push))))])
                 (declare! (field-name f) s))
               (check-push-size (clause 'push) bindings fields)
               fields)
             '()))
       (define varyings (if (clause 'varyings) (parse-varyings (clause 'varyings) declare!) '()))
       (define outputs
         (if (eq? kind 'graphics)
             (let* ([c (require-clause 'outputs)]
                    [fields (parse-fields (cdr (syntax->list c)) (format "the outputs of ~a" name) interface-problem)])
               (for ([f (in-list fields)] [s (in-list (cdr (syntax->list c)))])
                 (declare! (field-name f) s))
               fields)
             '()))
       (define m (material kind name shaders bindings push input varyings outputs workgroup (stx->loc stx)))
       (hash-set! materials name m)
       m]
      [_ (zgal-error 'malformed stx "expected (~a Name clause ...)" form-name)]))

  (define (parse-clauses stxs kind form-name)
    (define allowed
      (if (eq? kind 'graphics)
          '(shaders vertex-input bindings push varyings outputs)
          '(shaders workgroup bindings push)))
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

  (define (parse-shaders stx kind material-name)
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
        (match (hash-ref shader-names name #f)
          [(cons other where)
           (zgal-error 'shader-reused path-stx "~a is already a shader of material ~a (line ~a)"
                       name other (loc-line where))]
          [_ (hash-set! shader-names name (cons material-name (stx->loc path-stx)))])
        (shader resolved name stage (stx->loc path-stx))))
    (define stages (map shader-stage shaders))
    (define expected (if (eq? kind 'graphics) '(vertex fragment) '(compute)))
    (unless (and (= (length stages) (length expected))
                 (for/and ([s (in-list expected)]) (memq s stages)))
      (zgal-error 'bad-shader stx "~a needs ~a, but has ~a" material-name
                  (if (eq? kind 'graphics) "a .vert and a .frag shader" "one .comp shader")
                  (if (null? shaders) "none" (string-join (map shader-name shaders) " and "))))
    shaders)

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
      ['texture #:when (symbol? (syntax-e stx)) (values 'texture #f #f)]
      ['sampler #:when (symbol? (syntax-e stx)) (values 'sampler #f #f)]
      ['storage-image
       (match items
         [(list _ access) (values 'storage-image #f (parse-access access "storage image"))]
         [_ (zgal-error 'bad-binding stx "binding ~a: expected (storage-image access), with access read, write or read-write" name)])]
      [(or 'buffer 'buffer-array)
       (match items
         [(list _ type-stx access)
          (define t (parse-type type-stx))
          (when (array-t? t)
            (zgal-error 'type-not-allowed type-stx
                        "binding ~a: a ~a holds a struct, scalar, vector or matrix; for an array of them use (buffer-array type access)"
                        name head))
          ;; A buffer-array's elements are laid out like an array's
          (define problem (std430-problem (if (eq? head 'buffer-array) (array-t t 1) t)))
          (when problem (zgal-error 'type-not-allowed type-stx "binding ~a: ~a" name problem))
          (values head t (parse-access access (symbol->string head)))]
         [_ (zgal-error 'bad-binding stx "binding ~a: expected (~a type access), with access read, write or read-write"
                        name head)])]
      [_ (zgal-error 'bad-binding stx
                     "binding ~a: unknown kind ~a; expected texture, sampler, (storage-image access), (buffer type access) or (buffer-array type access)"
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

  (define-values (structs-out formats-out materials-out graphs-out)
    (for/fold ([ss '()] [fs '()] [ms '()] [gs '()]
               #:result (values (reverse ss) (reverse fs) (reverse ms) (reverse gs)))
              ([form (in-list forms)])
      (define items (syntax->list form))
      (match (head-symbol form)
        ['struct (values (cons (parse-struct form items) ss) fs ms gs)]
        ['vertex-format (values ss (cons (parse-vertex-format form items) fs) ms gs)]
        ['graphics-material (values ss fs (cons (parse-material form items 'graphics) ms) gs)]
        ['compute-material (values ss fs (cons (parse-material form items 'compute) ms) gs)]
        ['graph (values ss fs ms (cons (parse-graph form parse-type top-level materials) gs))]
        [_ (zgal-error 'unknown-form form
                       "expected struct, vertex-format, graphics-material, compute-material or graph, found ~a"
                       (let ([head (head-symbol form)]) (if head (format "(~a ...)" head) (show form))))])))
  (program source structs-out formats-out materials-out graphs-out))

;; Vertex inputs are named "in" followed by the attribute, e.g. inPosition
(define (input-variable-name attribute)
  (string-append "in" (string-upcase (substring attribute 0 1)) (substring attribute 1)))
(provide input-variable-name)
