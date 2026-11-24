#lang racket/base
;; Printing lowered bodies (lower.rkt) as GLSL or C++. The two differ in how they spell types,
;; literals, constructors, swizzles and built-in functions; the statements are the same.

(require racket/list
         racket/match
         racket/string
         "ast.rkt"
         "body.rkt"
         (only-in "builtins.rkt" component-kind)
         "cpp-common.rkt"
         "lower.rkt"
         "writer.rkt")

(provide spell-type
         nonuniform-texture-parameters
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
    [((builder-t e) 'cpp) (format "std::vector<~a>" (spell-type e dialect))]
    [((opaque-t k) 'glsl) (symbol->string k)]
    [((opaque-t 'path) 'cpp) "std::filesystem::path"]
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

;; Whether e is an element of an array binding, (at binding i)
(define (array-element? e)
  (match e
    [(t-index (t-global 'binding _ _ (binding-array-t _) _) _ _ _) #t]
    [_ #f]))

;; Whether a function's texture parameter may hold an array's element: the native (bindless)
;; interface, where callers pass elements; the web GLSL interface has no arrays (nor the
;; GL_EXT_nonuniform_qualifier that bindless.glsl enables)
(define nonuniform-texture-parameters (make-parameter #f))

;; Whether texture expression e is a function's texture parameter (texture locals are only those;
;; let* substitutes) that a caller may have given an array's element
(define (nonuniform-parameter? e)
  (and (nonuniform-texture-parameters) (t-local? e) (equal? (texpr-type e) (opaque-t 'texture2D))))

;; Whether combining texture expression e with a sampler needs nonuniformEXT: an array's element,
;; or such a parameter
(define (maybe-nonuniform-texture? e)
  (or (array-element? e) (nonuniform-parameter? e)))

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
    ;; glm's findMSB of a negative int is 31, GLSL's the most significant bit clear: of the
    ;; complement (the sign bit's copies make v ^ (v >> 31) it)
    [(t-call 'findMSB (list a) _ _)
     #:when (and (eq? dialect 'cpp) (eq? (component-kind (texpr-type a)) 'int))
     (format "[](auto v) { return glm::findMSB(v ^ (v >> 31)); }(~a)" (p a))]
    [(t-call f as _ _) (format "~a~a(~a)" (if (eq? dialect 'cpp) "glm::" "") f (args as))]
    [(t-user-call f as _ _) (format "~a(~a)" f (args as))]
    [(t-texture op as _ _)
     ;; A texture combined with a sampler to sample or fetch. An array's element is nonuniformEXT
     ;; again: the qualifier on its index reaches the texture, but not the combined sampler built
     ;; from it, nor the image texelFetch takes back out of that, so implementations that don't see
     ;; it may use one invocation's element for all (lavapipe's fetches did). So is a function's
     ;; texture parameter, which a caller may have given an element: the qualifier doesn't cross
     ;; calls either (on a uniform texture it costs little)
     (define (combined type t s)
       (define c (format "~a(~a, ~a)" type (p t) (p s)))
       (if (maybe-nonuniform-texture? t) (format "nonuniformEXT(~a)" c) c))
     (match* (op as)
       [('sample (list t s uv)) (format "texture(~a, ~a)" (combined "sampler2D" t s) (p uv))]
       [('sample-lod (list t s uv lod)) (format "textureLod(~a, ~a, ~a)" (combined "sampler2D" t s) (p uv) (p lod))]
       [('fetch (list t s texel lod)) (format "texelFetch(~a, ~a, ~a)" (combined "sampler2D" t s) (p texel) (p lod))]
       ;; Not a comparison's: SPIRV-Cross (MoltenVK) only finds the depth texture a comparison samples
       ;; through an unqualified combined sampler of the loaded texture (a NonUniform copy of either
       ;; makes it declare texture2d, which can't sample_compare). Mesa (lavapipe) makes a sample
       ;; nonuniform when either of OpSampledImage's operands is NonUniform: an element's load is,
       ;; and a parameter's sampler is qualified for it
       [('sample-compare (list t s uv ref))
        (format "texture(sampler2DShadow(~a, ~a), vec3(~a, ~a))"
                (p t) (if (nonuniform-parameter? t) (format "nonuniformEXT(~a)" (p s)) (p s)) (p uv) (p ref))]
       ;; Samplerless (GL_EXT_samplerless_texture_functions), so it works with any sampler
       ;; A parameter's size query is nonuniformEXT too, as the qualifier doesn't cross the call (an
       ;; element's queries the element's load, which its index qualifies)
       [('texture-size (list t lod))
        (format "textureSize(~a, ~a)" (if (nonuniform-parameter? t) (format "nonuniformEXT(~a)" (p t)) (p t)) (p lod))]
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
    ;; collect's elements, made immutable data
    [(t-construct (list b) (data-t el) _) (format "zgal::Data<~a>(std::move(~a))" (spell-type el dialect) (p b))]
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
    [(t-effect 'resize (list w h) _) (format "resize_ = VkExtent2D{~a, ~a}" (p w) (p h))]
    [(t-effect 'quit '() _) "quit_ = true"]
    [(t-effect 'append (list b v) _) (format "~a.push_back(~a)" (p b) (p v))]
    ;; The app class's function running the node again in each graph that has it
    [(t-effect 'run-once (list (t-lit node _ _)) _) (format "runOnce_~a()" node)]))

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
