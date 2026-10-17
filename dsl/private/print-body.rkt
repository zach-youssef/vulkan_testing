#lang racket/base
;; Printing lowered bodies (lower.rkt) as GLSL or C++. The two differ in how they spell types,
;; literals, constructors, swizzles and built-in functions; the statements are the same.

(require racket/list
         racket/match
         racket/string
         "ast.rkt"
         "body.rkt"
         "cpp-common.rkt"
         "lower.rkt"
         "writer.rkt")

(provide spell-type
         print-expression
         emit-statements!
         emit-function!
         emit-constant!)

;; dialect: 'glsl or 'cpp

(define (spell-type t dialect)
  (match* (t dialect)
    [((array-t e n) 'glsl) (format "~a[~a]" (spell-type e dialect) n)]
    [((array-t e n) 'cpp) (format "std::array<~a, ~a>" (spell-type e dialect) n)]
    [((void-t) _) "void"]
    [((data-t e) 'cpp) (format "zgal::Data<~a>" (spell-type e dialect))]
    [((opaque-t k) 'glsl) (symbol->string k)]
    [((scalar-t k) 'glsl) #:when (small-integer-kind? k) (format "~a_t" k)]
    [((scalar-t 'bool) 'cpp) "bool"]
    [((vector-t 'bool n) 'cpp) (format "glm::bvec~a" n)]
    [(_ 'cpp) (cpp-type t)]
    [(_ 'glsl) (type-name t)]))

(define (literal v t dialect)
  (match t
    [(opaque-t 'string) (cpp-string v)]
    [(scalar-t 'bool) (if v "true" "false")]
    [(scalar-t 'float)
     (define text (number->string (exact->inexact v)))
     (define decimal (if (regexp-match? #rx"[.e]" text) text (string-append text ".0")))
     (if (eq? dialect 'cpp) (string-append decimal "f") decimal)]
    [(scalar-t 'uint) (format "~au" v)]
    [(scalar-t k) #:when (small-integer-kind? k) (format "~a(~a)" (spell-type t dialect) v)]
    [_ (format "~a" v)]))

(define glsl-operators
  (hash '+ "+" '- "-" '* "*" '/ "/" 'remainder "%" 'bitand "&" 'bitor "|" 'bitxor "^" 'shl "<<" 'shr ">>"
        '< "<" '<= "<=" '> ">" '>= ">=" '= "==" 'not= "!=" 'and "&&" 'or "||"))

;; Whether e prints as a single operand (no parentheses needed around it)
(define (atomic? e)
  (match e
    [(or (t-lit _ _ _) (t-local _ _ _) (t-global _ _ _ _ _) (t-call _ _ _ _) (t-user-call _ _ _ _)
         (t-texture _ _ _ _) (t-construct _ _ _) (t-field _ _ _ _) (t-index _ _ _ _)) #t]
    [(t-swizzle _ c _ _) #t]
    [_ #f]))

(define (print-expression e dialect)
  (define (p e) (print-expression e dialect))
  (define (operand e) (if (atomic? e) (p e) (format "(~a)" (p e))))
  (define (args es) (string-join (map p es) ", "))
  (define (constructor t) (spell-type t dialect))
  (match e
    [(t-lit v t _) (literal v t dialect)]
    [(t-local name _ _) name]
    [(t-global kind name spelling _ _)
     (case kind
       [(binding push) (format "~a()" name)]
       [(input stage host) spelling]
       [else name])]
    [(t-call 'not (list a) _ _) (if (eq? dialect 'glsl) (format "not(~a)" (p a)) (format "glm::not_(~a)" (p a)))]
    [(t-call f as _ _) (format "~a~a(~a)" (if (eq? dialect 'cpp) "glm::" "") f (args as))]
    [(t-user-call f as _ _) (format "~a(~a)" f (args as))]
    [(t-texture op as _ _)
     (match* (op as)
       [('sample (list t s uv)) (format "texture(sampler2D(~a, ~a), ~a)" (p t) (p s) (p uv))]
       [('sample-lod (list t s uv lod)) (format "textureLod(sampler2D(~a, ~a), ~a, ~a)" (p t) (p s) (p uv) (p lod))]
       [('fetch (list t s texel lod)) (format "texelFetch(sampler2D(~a, ~a), ~a, ~a)" (p t) (p s) (p texel) (p lod))]
       [('sample-compare (list t s uv ref)) (format "texture(sampler2DShadow(~a, ~a), vec3(~a, ~a))" (p t) (p s) (p uv) (p ref))]
       ;; Samplerless (GL_EXT_samplerless_texture_functions), so it works with any sampler
       [('texture-size (list t lod)) (format "textureSize(~a, ~a)" (p t) (p lod))]
       [('image-load (list i texel)) (format "imageLoad(~a, ~a)" (p i) (p texel))]
       [('image-size (list i)) (format "imageSize(~a)" (p i))])]
    [(t-op 'negate (list a) _ _) (format "-~a" (operand a))]
    [(t-op 'not (list a) _ _) (format "!~a" (operand a))]
    [(t-op 'bitnot (list a) _ _) (format "~~~a" (operand a))]
    [(t-op 'array-length (list (t-global _ name _ _ _)) _ _) (format "zgalLength_~a()" name)]
    [(t-op 'data-count (list d) _ _) (format "~a.size()" (operand d))]
    [(t-op op as _ _) (string-join (map operand as) (format " ~a " (hash-ref glsl-operators op)))]
    [(t-construct as (struct-t d) _)
     (if (eq? dialect 'glsl)
         (format "~a(~a)" (struct-decl-name d) (args as))
         (format "~a{~a}" (struct-decl-name d)
                 (string-join (for/list ([f (in-list (struct-decl-fields d))] [a (in-list as)])
                                (format ".~a = ~a" (field-name f) (p a)))
                              ", ")))]
    [(t-construct as (array-t el n) _)
     (if (eq? dialect 'glsl)
         (format "~a[](~a)" (spell-type el dialect) (args as))
         (format "std::array<~a, ~a>{~a}" (spell-type el dialect) n (args as)))]
    [(t-construct (list a) (scalar-t k) _)
     (if (eq? dialect 'glsl)
         (format "~a(~a)" (constructor (scalar-t k)) (p a))
         (format "static_cast<~a>(~a)" (constructor (scalar-t k)) (p a)))]
    [(t-construct as t _) (format "~a(~a)" (constructor t) (args as))]
    [(t-field b name _ _) (format "~a.~a" (operand b) name)]
    [(t-swizzle b comps _ _)
     (if (or (eq? dialect 'glsl) (= (string-length comps) 1))
         (format "~a.~a" (operand b) comps)
         ;; GLM has no swizzles without GLM_FORCE_SWIZZLE: build the vector
         (format "~a(~a)" (spell-type (texpr-type e) dialect)
                 (string-join (for/list ([c (in-string comps)]) (format "~a.~a" (operand b) c)) ", ")))]
    ;; An array binding's element: its accessor takes the index
    [(t-index (t-global 'binding name _ (binding-array-t _) _) i _ _) (format "~a(~a)" name (p i))]
    [(t-index b i _ _) (format "~a[~a]" (operand b) (p i))]
    [(t-if c a b _ _) (format "~a ? ~a : ~a" (operand c) (operand a) (operand b))]
    [(t-store i texel v _) (format "imageStore(~a, ~a, ~a)" (p i) (p texel) (p v))]
    [(t-barrier _ _) "barrier()"]
    [(t-exit op _) (symbol->string op)]
    [(t-atomic op place as _ _)
     (format "~a(~a, ~a)"
             (case op
               [(add) "atomicAdd"] [(min) "atomicMin"] [(max) "atomicMax"] [(and) "atomicAnd"] [(or) "atomicOr"]
               [(xor) "atomicXor"] [(exchange) "atomicExchange"] [(compare-exchange) "atomicCompSwap"])
             (p place) (args as))]
    ;; Apps' effects, in the C++ of a generated app class (cpp-app.rkt), whose members they use
    [(t-effect 'print as _)
     (format "std::cout << ~a << std::endl"
             (string-join (for/list ([a (in-list as)])
                            (if (equal? (texpr-type a) (scalar-t 'bool))
                                (format "(~a ? \"true\" : \"false\")" (p a))
                                (p a)))
                          " << "))]
    [(t-effect 'set-frames-in-flight (list n) _) (format "context_->setFramesInFlight(~a)" (p n))]
    [(t-effect 'quit '() _) "quit_ = true"]))

(define (declaration name t dialect)
  (match* (t dialect)
    [((array-t e n) 'glsl) (format "~a ~a[~a]" (spell-type e dialect) name n)]
    [(_ _) (format "~a ~a" (spell-type t dialect) name)]))

;; Emits statements, each at its .zgal location
(define (emit-statements! w stmts dialect indent)
  (define (line text where) (emit! w (string-append indent text) #:at where))
  (define (p e) (print-expression e dialect))
  (for ([s (in-list stmts)])
    (match s
      [(s-declare name t v l)
       (line (if v (format "~a = ~a;" (declaration name t dialect) (p v)) (format "~a;" (declaration name t dialect))) l)]
      [(s-assign place v l) (line (format "~a = ~a;" (p place) (p v)) l)]
      [(s-if c then else l)
       (line (format "if (~a) {" (p c)) l)
       (emit-statements! w then dialect (string-append indent "    "))
       (cond
         [(null? else) (line "}" l)]
         [else
          (line "} else {" l)
          (emit-statements! w else dialect (string-append indent "    "))
          (line "}" l)])]
      [(s-for name t from to step body l)
       (line (format "for (~a = ~a; ~a < ~a; ~a) {" (declaration name t dialect) (p from) name (p to)
                     (match step
                       [(t-lit 1 _ _) (format "++~a" name)]
                       [_ (format "~a += ~a" name (p step))]))
             l)
       (emit-statements! w body dialect (string-append indent "    "))
       (line "}" l)]
      [(s-effect e l) (line (format "~a;" (p e)) l)]
      [(s-while cs c body l)
       (define inner (string-append indent "    "))
       (cond
         [(null? cs) (line (format "while (~a) {" (p c)) l)]
         [else
          ;; The condition needs statements first: test it inside the loop
          (line "while (true) {" l)
          (emit-statements! w cs dialect inner)
          (emit! w (format "~aif (!(~a)) {" inner (p c)) #:at l)
          (emit! w (format "~a    break;" inner) #:at l)
          (emit! w (format "~a}" inner) #:at l)])
       (emit-statements! w body dialect inner)
       (line "}" l)])))

;; A counter of temporaries' spellings for one function
(define (fresh-maker)
  (define n 0)
  (λ (base)
    (set! n (add1 n))
    (format "zgal~a~a" (string-titlecase base) n)))

(define (emit-function! w f dialect)
  (define fresh (fresh-maker))
  (define-values (stmts value) (lower (function-decl-body f) fresh))
  (define l (function-decl-loc f))
  (emit! w (format "~a~a ~a(~a) {" (if (eq? dialect 'cpp) "inline " "")
                   (spell-type (function-decl-result f) dialect) (function-decl-name f)
                   (string-join (for/list ([p (in-list (function-decl-params f))]) (declaration (car p) (cdr p) dialect)) ", "))
         #:at l)
  (emit-statements! w stmts dialect "    ")
  (when value
    (emit! w (format "    return ~a;" (print-expression value dialect)) #:at (texpr-loc (function-decl-body f))))
  (emit! w "}" #:at l))

(define (emit-constant! w c dialect)
  (define-values (stmts value) (lower (constant-decl-body c) (fresh-maker)))
  (emit! w (format "~a ~a = ~a;" (if (eq? dialect 'cpp) "inline const" "const")
                   (declaration (constant-decl-name c) (constant-decl-type c) dialect) (print-expression value dialect))
         #:at (constant-decl-loc c)))
