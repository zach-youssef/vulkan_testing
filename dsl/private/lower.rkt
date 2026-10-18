#lang racket/base
;; Lowering typed expressions (body.rkt) to statements: GLSL and C++ are statement languages, so
;; let*, folds, sequences and effects become declarations, loops, ifs and assignments, leaving
;; expressions without them. Backend independent: glsl-body.rkt and cpp-function.rkt print it.

(require racket/list
         racket/match
         "ast.rkt"
         "body.rkt"
         "errors.rkt")

(provide (struct-out s-declare)
         (struct-out s-assign)
         (struct-out s-if)
         (struct-out s-for)
         (struct-out s-effect)
         (struct-out s-while)
         lower
         lower-function
         lower-stage)

;; Statements. Every one has a loc, where it came from in the .zgal file.
(struct s-declare (name type value loc) #:transparent)     ; value: an expression, or #f
(struct s-assign (place value loc) #:transparent)          ; place: a t-local, or a global binding's field or element
(struct s-if (condition then else loc) #:transparent)     ; then, else: statement lists
(struct s-for (name type from to step body loc) #:transparent)  ; for (name = from; name < to; name += step)
(struct s-effect (expression loc) #:transparent)           ; an expression evaluated for its effect
;; while: the condition's statements, then it; while it holds, body
(struct s-while (condition-statements condition body loc) #:transparent)

;; Lowers e: returns statements to run first, then the expression giving its value (a list of
;; expressions for several values; #f for no value). fresh: makes a temporary's spelling.
(define (lower e fresh)
  (define (go e) (lower e fresh))
  ;; Lowers expressions in order, combining their statements
  (define (go* es)
    (for/fold ([stmts '()] [vals '()] #:result (values stmts (reverse vals)))
              ([e (in-list es)])
      (define-values (s v) (go e))
      (values (append stmts s) (cons v vals))))
  (define here (texpr-loc e))
  (match e
    [(or (t-lit _ _ _) (t-local _ _ _) (t-global _ _ _ _ _)) (values '() e)]
    ;; A barrier for buffers makes buffer writes before it visible to the workgroup after it
    [(t-barrier #t l) (values (list (s-effect (t-call 'memoryBarrierBuffer '() (void-t) l) l) (s-effect (t-barrier #f l) l)) #f)]
    [(or (t-barrier _ l) (t-exit _ l)) (values (list (s-effect e l)) #f)]
    [(t-atomic op place args t l)
     ;; Into a temporary, so it happens once, in order, whether or not its value is used
     (define-values (ps pv) (go place))
     (define-values (as avs) (go* args))
     (define tmp (fresh "atomic"))
     (values (append ps as (list (s-declare tmp t (t-atomic op pv avs t l) l))) (t-local tmp t l))]
    [(t-effect op args l)
     (let-values ([(s vs) (go* args)]) (values (append s (list (s-effect (t-effect op vs l) l))) #f))]
    [(t-call f args t l) (let-values ([(s vs) (go* args)]) (values s (t-call f vs t l)))]
    [(t-user-call f args t l)
     (define-values (s vs) (go* args))
     (if (void-t? t)
         (values (append s (list (s-effect (t-user-call f vs t l) l))) #f)
         (values s (t-user-call f vs t l)))]
    [(t-texture op args t l) (let-values ([(s vs) (go* args)]) (values s (t-texture op vs t l)))]
    [(t-op op args t l) (let-values ([(s vs) (go* args)]) (values s (t-op op vs t l)))]
    [(t-construct args t l) (let-values ([(s vs) (go* args)]) (values s (t-construct vs t l)))]
    [(t-field b n t l) (let-values ([(s v) (go b)]) (values s (t-field v n t l)))]
    [(t-swizzle b c t l) (let-values ([(s v) (go b)]) (values s (t-swizzle v c t l)))]
    [(t-index b i t l) (let-values ([(s vs) (go* (list b i))]) (values s (t-index (first vs) (second vs) t l)))]
    [(t-if c a b t l)
     (define-values (cs cv) (go c))
     (define-values (as av) (go a))
     (cond
       [(not b)
        ;; when: effects only
        (values (append cs (list (s-if cv as '() l))) #f)]
       [else
        (define-values (bs bv) (go b))
        (cond
          [(void-t? t) (values (append cs (list (s-if cv as bs l))) #f)]
          [(and (null? as) (null? bs) (not (list? av))) (values cs (t-if cv av bv t l))]
          [(list? av)
           ;; Several values (a fold's or while's next ones): a temporary each
           (define tmps (for/list ([ty (in-list (values-t-types t))]) (t-local (fresh "branch") ty l)))
           (define (assign vs) (for/list ([tmp (in-list tmps)] [v (in-list vs)]) (s-assign tmp v l)))
           (values (append cs (for/list ([tmp (in-list tmps)]) (s-declare (t-local-name tmp) (t-local-type tmp) #f l))
                           (list (s-if cv (append as (assign av)) (append bs (assign bv)) l)))
                   tmps)]
          [else
           ;; Branches with statements assign a temporary
           (define tmp (t-local (fresh "branch") t l))
           (values (append cs (list (s-declare (t-local-name tmp) t #f l)
                                    (s-if cv (append as (list (s-assign tmp av l)))
                                          (append bs (list (s-assign tmp bv l))) l)))
                   tmp)])])]
    [(t-let bindings body t l)
     (define stmts
       (append*
        (for/list ([b (in-list bindings)])
          (match-define (list names types value) b)
          (define-values (s v) (go value))
          (append s
                  (if (= (length names) 1)
                      (list (s-declare (first names) (first types) v (texpr-loc value)))
                      (for/list ([n (in-list names)] [ty (in-list types)] [x (in-list v)])
                        (s-declare n ty x (texpr-loc value))))))))
     (define-values (bs bv) (go body))
     (values (append stmts bs) bv)]
    [(t-values es t l) (go* es)]
    [(t-begin es t l)
     (define-values (s vs) (go* es))
     ;; Only the last value is kept; the others are effects, already in s
     (values s (last vs))]
    [(t-fold accs loops body t l)
     (define-values (init-s init-v) (go* (map third accs)))
     (define declares
       (for/list ([a (in-list accs)] [v (in-list init-v)])
         (s-declare (first a) (second a) v l)))
     (define-values (bs bv) (go body))
     (define update (accumulator-updates accs bv fresh l))
     (define-values (loop-s loop) (nest loops (append bs update) go l))
     (values (append init-s declares loop-s (list loop))
             (let ([vars (for/list ([a (in-list accs)]) (t-local (first a) (second a) l))])
               (if (= (length vars) 1) (first vars) vars)))]
    [(t-while accs c body t l)
     (define-values (init-s init-v) (go* (map third accs)))
     (define declares
       (for/list ([a (in-list accs)] [v (in-list init-v)])
         (s-declare (first a) (second a) v l)))
     (define-values (cs cv) (go c))
     (define-values (bs bv) (go body))
     (define update (accumulator-updates accs bv fresh l))
     (values (append init-s declares (list (s-while cs cv (append bs update) l)))
             (let ([vars (for/list ([a (in-list accs)]) (t-local (first a) (second a) l))])
               (if (= (length vars) 1) (first vars) vars)))]
    [(t-for loops body l)
     (define-values (bs _) (go body))
     (define-values (loop-s loop) (nest loops bs go l))
     (values (append loop-s (list loop)) #f)]
    [(t-store image texel value l)
     (define-values (s vs) (go* (list image texel value)))
     (values (append s (list (s-effect (t-store (first vs) (second vs) (third vs) l) l))) #f)]
    [(t-write place value l)
     (define-values (ps pv) (go place))
     (define-values (vs vv) (go value))
     (values (append ps vs (list (s-assign pv vv l))) #f)]
    [(t-with base fields t l)
     (define-values (bs bv) (go base))
     (define tmp (t-local (fresh "updated") t l))
     (define-values (fs fvs) (go* (map cdr fields)))
     (values (append bs fs (list (s-declare (t-local-name tmp) t bv l))
                     (for/list ([f (in-list fields)] [v (in-list fvs)])
                       (s-assign (t-field tmp (car f) (texpr-type (cdr f)) l) v l)))
             tmp)]
    [(t-results fields l)
     (define-values (s vs) (go* (map cdr fields)))
     (values s (for/list ([f (in-list fields)] [v (in-list vs)]) (cons (car f) v)))]))

;; Statements giving accumulators (list name type init) their new values (one expression, or a list)
(define (accumulator-updates accs new fresh l)
  (define new-values (if (list? new) new (list new)))
  (if (= (length accs) 1)
      (list (s-assign (t-local (first (first accs)) (second (first accs)) l) (first new-values) l))
      ;; Several accumulators update at once: compute every new value before assigning any
      (let ([tmps (for/list ([a (in-list accs)]) (fresh (format "~aNext" (first a))))])
        (append (for/list ([a (in-list accs)] [tmp (in-list tmps)] [v (in-list new-values)])
                  (s-declare tmp (second a) v l))
                (for/list ([a (in-list accs)] [tmp (in-list tmps)])
                  (s-assign (t-local (first a) (second a) l) (t-local tmp (second a) l) l))))))

;; Loops nested in order around body. The bounds are computed before the loops (an inner loop's
;; bounds can use outer variables, so they're computed in the outer loop's body).
(define (nest loops body go l)
  (match-define (cons (list name from to step) more) loops)
  (define-values (fs fv) (go from))
  (define-values (ts tv) (go to))
  (define-values (ss sv) (go step))
  (define inner-body
    (if (null? more)
        body
        (let-values ([(inner-s inner) (nest more body go l)]) (append inner-s (list inner)))))
  (values (append fs ts ss) (s-for name (texpr-type from) fv tv sv inner-body l)))

;; A function body: statements and the returned expression (#f for void)
(define (lower-function body fresh)
  (lower body fresh))

;; A stage body: statements, then its results as (cons name expression) (or one expression for a
;; fragment shader with one output, or #f)
(define (lower-stage body fresh)
  (lower body fresh))
