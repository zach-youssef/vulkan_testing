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
    [((opaque-t k) 'glsl) (symbol->string k)]
    [((scalar-t 'bool) 'cpp) "bool"]
    [((vector-t 'bool n) 'cpp) (format "glm::bvec~a" n)]
    [(_ 'cpp) (cpp-type t)]
    [(_ 'glsl) (type-name t)]))

(define (literal v t dialect)
  (match t
    [(scalar-t 'bool) (if v "true" "false")]
    [(scalar-t 'float)
     (define text (number->string (exact->inexact v)))
     (define decimal (if (regexp-match? #rx"[.e]" text) text (string-append text ".0")))
     (if (eq? dialect 'cpp) (string-append decimal "f") decimal)]
    [(scalar-t 'uint) (format "~au" v)]
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
       [(input stage) spelling]
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
    [(t-index b i _ _) (format "~a[~a]" (operand b) (p i))]
    [(t-if c a b _ _) (format "~a ? ~a : ~a" (operand c) (operand a) (operand b))]
    [(t-store i texel v _) (format "imageStore(~a, ~a, ~a)" (p i) (p texel) (p v))]))

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
      [(s-for name t from to body l)
       (line (format "for (~a = ~a; ~a < ~a; ++~a) {" (declaration name t dialect) (p from) name (p to) name) l)
       (emit-statements! w body dialect (string-append indent "    "))
       (line "}" l)]
      [(s-effect e l) (line (format "~a;" (p e)) l)])))

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
