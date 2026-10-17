#lang racket/base
;; What a program needs of the device beyond what the wrapper requires: 8- and 16-bit integers, float
;; atomics and subgroup operations. The GLSL backend enables their extensions, and the C++ backend
;; reports the device features each material (and graph) needs, and checks the device's subgroups
;; support what its shaders use.

(require racket/list
         racket/match
         "ast.rkt"
         "body.rkt"
         "builtins.rkt"
         "layout.rkt")

(provide program-glsl-extensions
         material-feature-flags
         material-subgroup-needs)

;; Everything in e, including itself
(define (subexpressions e)
  (cons e (append-map subexpressions (texpr-children e))))

;; The types of e and everything in it, including locals and accumulators
(define (texpr-types e)
  (append*
   (for/list ([x (in-list (subexpressions e))])
     (append
      (let ([t (texpr-type x)]) (if (or (void-t? t) (results-t? t) (values-t? t)) '() (list t)))
      (match x
        [(t-let bindings _ _ _) (append-map second bindings)]
        [(or (t-fold accs _ _ _ _) (t-while accs _ _ _ _)) (map second accs)]
        [_ '()])))))

(define (widths-of types)
  (sort (remove-duplicates
         (for*/list ([t (in-list types)] [k (in-list (small-integer-kinds-in t))])
           (small-integer-bits k)))
        <))

(define (declarations-by-name prog)
  (for/hash ([d (in-list (program-functions prog))])
    (values (if (function-decl? d) (function-decl-name d) (constant-decl-name d)) d)))

;; The declarations (functions and constants) m's ZGAL shaders use
(define (material-declarations m prog)
  (define declarations (declarations-by-name prog))
  (for*/list ([s (in-list (material-shaders m))] #:when (zgal-shader? s)
              [n (in-list (zgal-shader-uses s))])
    (hash-ref declarations n)))

;; The bodies m's shaders run, with their stages: its ZGAL bodies and the functions they use (GLSL
;; bodies and GLSL functions can't be looked into)
(define (material-bodies m prog)
  (define declarations (declarations-by-name prog))
  (for*/list ([s (in-list (material-shaders m))] #:when (zgal-shader? s)
              [b (in-list (cons (zgal-shader-body s)
                                (for*/list ([n (in-list (zgal-shader-uses s))]
                                            [d (in-value (hash-ref declarations n))]
                                            #:when (and (function-decl? d) (function-decl-body d)))
                                  (function-decl-body d))))])
    (cons (shader-stage s) b)))

(define (declaration-types d)
  (match d
    [(function-decl _ params result _ _ _ _) (append (map cdr params) (list result))]
    [(constant-decl _ type _ _ _ _) (list type)]))

;; The integer widths (8, 16) m's shaders use: its buffers' and shared variables' types, and what
;; its ZGAL bodies compute with
(define (material-integer-widths m prog)
  (widths-of
   (append
    (for/list ([b (in-list (material-bindings m))] #:when (binding-buffer-type b)) (binding-buffer-type b))
    (map field-type (material-shared m))
    (append-map declaration-types (material-declarations m prog))
    (append-map (λ (sb) (texpr-types (cdr sb))) (material-bodies m prog)))))

;; The float atomics in m's bodies, as VK_EXT_shader_atomic_float feature names
(define (material-atomic-float-features m prog)
  (remove-duplicates
   (for*/list ([sb (in-list (material-bodies m prog))]
               [x (in-list (subexpressions (cdr sb)))]
               #:when (and (t-atomic? x) (equal? (t-atomic-type x) (scalar-t 'float))))
     (define shared? (let root ([p (t-atomic-place x)])
                       (match p
                         [(t-global kind _ _ _ _) (eq? kind 'shared)]
                         [(or (t-field b _ _ _) (t-index b _ _ _)) (root b)])))
     (format "atomicFloat.shader~aFloat32Atomic~a" (if shared? "Shared" "Buffer")
             (if (eq? (t-atomic-op x) 'add) "Add" "s")))))

;; The device features m's shaders need beyond the wrapper's, as DeviceFeatures members, e.g.
;; "vulkan12.shaderInt8"
(define (material-feature-flags m prog)
  (append
   (append*
    (for/list ([bits (in-list (material-integer-widths m prog))])
      (if (= bits 8)
          '("vulkan12.shaderInt8" "vulkan12.storageBuffer8BitAccess")
          '("core.features.shaderInt16" "vulkan11.storageBuffer16BitAccess"))))
   (sort (material-atomic-float-features m prog) string<?)))

;; The stages using subgroups, and the operations they use ('basic 'arithmetic 'vote 'ballot
;; 'shuffle): (values stages categories)
(define (material-subgroup-needs m prog)
  (define uses
    (for*/list ([sb (in-list (material-bodies m prog))]
                [x (in-list (subexpressions (cdr sb)))]
                [category (in-value (match x
                                      [(t-call f _ _ _) (subgroup-category f)]
                                      [(t-global 'stage name _ _ _) #:when (regexp-match? #px"^subgroup" name) 'basic]
                                      [_ #f]))]
                #:when category)
      (cons (car sb) category)))
  (values (remove-duplicates (map car uses))
          (remove-duplicates (append (if (null? uses) '() '(basic)) (map cdr uses)))))

;; The GLSL extensions prog's shaders enable: those for what the file uses anywhere (enabling an
;; extension a shader doesn't use costs nothing: the SPIR-V only declares the capabilities it uses)
(define (program-glsl-extensions prog)
  (define bodies
    (append
     (for*/list ([m (in-list (program-materials prog))] [sb (in-list (material-bodies m prog))]) (cdr sb))
     (for/list ([d (in-list (program-functions prog))] #:when (and (function-decl? d) (function-decl-body d)))
       (function-decl-body d))))
  (define all (append-map subexpressions bodies))
  (define widths
    (widths-of
     (append
      (for*/list ([d (in-list (program-structs prog))] [f (in-list (struct-decl-fields d))]) (field-type f))
      (append-map declaration-types (program-functions prog))
      (for*/list ([m (in-list (program-materials prog))] [w (in-list (material-integer-widths m prog))])
        (scalar-t (if (= w 8) 'uint8 'uint16))))))
  (append
   (for/list ([bits (in-list widths)]) (format "GL_EXT_shader_explicit_arithmetic_types_int~a" bits))
   (if (ormap (λ (x) (and (t-atomic? x) (equal? (t-atomic-type x) (scalar-t 'float)))) all)
       '("GL_EXT_shader_atomic_float")
       '())
   (if (ormap (λ (x) (match x
                       [(t-call f _ _ _) (and (subgroup-category f) #t)]
                       [(t-global 'stage name _ _ _) (regexp-match? #px"^subgroup" name)]
                       [_ #f]))
              all)
       '("GL_KHR_shader_subgroup_basic" "GL_KHR_shader_subgroup_arithmetic" "GL_KHR_shader_subgroup_vote"
         "GL_KHR_shader_subgroup_ballot" "GL_KHR_shader_subgroup_shuffle")
       '())))
