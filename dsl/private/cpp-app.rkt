#lang racket/base
;; The C++ backend for apps: a class per app holding its state, running its handlers on events and
;; each frame, and building its graph with inputs computed from the state. Uses the runtime in
;; include/zgal/App.h; include/zgal/Window.h connects it to a VulkanApp.

(require racket/list
         racket/match
         racket/string
         "app.rkt"
         "ast.rkt"
         "body.rkt"
         "cpp-common.rkt"
         "cpp-graph.rkt"
         "lower.rkt"
         "print-body.rkt"
         "writer.rkt")

(provide emit-app!)

(define (event-struct kind)
  (case kind
    [(key-pressed) "KeyPressed"] [(key-released) "KeyReleased"]
    [(mouse-pressed) "MousePressed"] [(mouse-released) "MouseReleased"]
    [(cursor-moved) "CursorMoved"] [(scrolled) "Scrolled"]))

(define (key-enumerator key) (string-append "zgal::Key::" (cdr (assoc key key-names))))
(define (button-enumerator b) (case b [(left) "zgal::MouseButton::Left"] [(right) "zgal::MouseButton::Right"] [(middle) "zgal::MouseButton::Middle"]))

;; Emits a member function computing e: `signature {`, e's statements, `return value; }`. Bodies
;; read the state as `state`.
(define (emit-body-function! w signature e where #:state [state-line #f])
  (define n 0)
  (define (fresh base)
    (set! n (add1 n))
    (format "zgal~a~a" (string-titlecase base) n))
  (define-values (stmts value) (lower e fresh))
  (emit! w (format "    ~a {" signature) #:at where)
  (when state-line (emit! w (string-append "        " state-line) #:at where))
  (emit-statements! w stmts 'cpp "        ")
  (emit! w (format "        return ~a;" (print-expression value 'cpp)) #:at (texpr-loc e))
  (emit! w "    }" #:at where))

(define (lower-first s) (string-append (string-downcase (substring s 0 1)) (substring s 1)))

(define (emit-app! w prog a)
  (define name (app-name a))
  (define (gen text) (emit! w text))
  (define state (app-state a))
  (define runs (app-runs a))
  (define single? (= (length runs) 1))
  (define frame-handler (for/first ([h (in-list (app-handlers a))] #:when (eq? (event-kind (handler-event h)) 'frame)) h))
  (define read-handlers (filter (λ (h) (eq? (event-kind (handler-event h)) 'read)) (app-handlers a)))
  ;; Per run: its graph, the members naming it, and what C++ passes through to it
  (define (run-graph r) (graph-run-graph r))
  (define (run-index r) (index-of runs r))
  (define (graph-member r) (format "graph~a_" (run-index r)))
  (define (ids-member r) (format "ids~a_" (run-index r)))
  ;; A function computing something of a run: one run keeps the plain names
  (define (run-name r base) (if single? base (format "~a_~a" base (graph-name (run-graph r)))))
  (define (passthrough r)
    (for/list ([m (in-list (graph-input-members (run-graph r)))]
               #:unless (assoc (graph-item-name (car m)) (graph-run-bindings r)))
      m))
  (define (run-of-graph g-name) (findf (λ (r) (equal? (graph-name (run-graph r)) g-name)) runs))
  (define (node-of r node-name) (findf (λ (n) (equal? (g-node-name n) node-name)) (graph-nodes (run-graph r))))
  (define (handler-name i) (format "on~a" i))
  (define indexed (for/list ([h (in-list (app-handlers a))] [i (in-naturals)]) (cons h i)))
  (define (index-of-handler h) (cdr (assq h indexed)))

  (gen "")
  (gen (format "// app ~a: its state, the handlers computing the next one, and ~a" name
               (match runs
                 ['() "no graph"]
                 [(list r) (format "graph ~a" (graph-name (run-graph r)))]
                 [_ (format "graphs ~a, one active each frame" (string-join (map (λ (r) (graph-name (run-graph r))) runs) ", "))])))
  (emit! w (format "class ~a {" name) #:at (app-loc a))
  (gen "public:")
  (gen "    // What the handlers compute the next of")
  (emit! w "    struct State {" #:at (struct-decl-loc state))
  (for ([f (in-list (struct-decl-fields state))])
    (emit! w (format "        ~a{};" (declaration-cpp (field-type f) (field-name f))) #:at (field-loc f)))
  (gen "    };")
  (gen "")
  (gen "    // What C++ supplies: the parameters, the clock, and graph inputs the app doesn't compute")
  (gen "    struct Inputs {")
  (for ([p (in-list (app-parameters a))])
    (emit! w (format "        ~a{};" (declaration-cpp (field-type p) (field-name p))) #:at (field-loc p)))
  (when frame-handler
    (gen "        zgal::Timestep timestep;   // How far the frame handler's clock advances a frame"))
  (for ([f (in-list (app-externs a))])
    (emit! w (format "        std::function<~a(~a)> ~a;" (spell-type (function-decl-result f) 'cpp)
                     (string-join (for/list ([p (in-list (function-decl-params f))]) (extern-parameter-type (cdr p))) ", ")
                     (function-decl-name f))
           #:at (function-decl-loc f)))
  (for* ([r (in-list runs)] [m (in-list (passthrough r))])
    (emit! w (string-append "        " (cdr m)) #:at (item-loc (car m))))
  (gen "    };")
  (gen "")
  (unless single?
    (unless (null? runs)
      (gen "    // The render graphs it adds its graphs to (e.g. VulkanApp::createGraph), one per graph")
      (gen "    struct Graphs {")
      (for ([r (in-list runs)])
        (emit! w (format "        RenderGraph* ~a = nullptr;" (lower-first (graph-name (run-graph r)))) #:at (graph-run-loc r)))
      (gen "    };")
      (gen "")))

  ;; create
  (define graphs-parameter
    (if (or single? (null? runs)) "RenderGraph& graph" "Graphs graphs"))
  (gen (format "    // The app, with its initial state~a. The graphs' callbacks"
               (cond [(null? runs) ""] [single? ", adding its graph to `graph`"] [else ", adding each graph to its render graph"])))
  (gen "    // use the app, so it must outlive their use.")
  (gen (format "    static Result<std::unique_ptr<~a>> create(VulkanContext& context, ~a, Inputs inputs) {" name graphs-parameter))
  (when frame-handler
    (gen "        if (!inputs.timestep.given()) {")
    (gen (format "            contractViolation(~a);" (cpp-string (format "~a's timestep isn't given: zgal::Timestep::realTime or fixed" name))))
    (gen "        }"))
  (for ([f (in-list (app-externs a))])
    (emit! w (format "        if (!inputs.~a) {" (function-decl-name f)) #:at (function-decl-loc f))
    (gen (format "            contractViolation(~a);" (cpp-string (format "~a's extern function ~a isn't given" name (function-decl-name f)))))
    (gen "        }"))
  (unless (or single? (null? runs))
    (for ([r (in-list runs)])
      (define member (lower-first (graph-name (run-graph r))))
      (gen (format "        if (!graphs.~a) {" member))
      (gen (format "            contractViolation(~a);" (cpp-string (format "~a's render graph for ~a isn't given" name (graph-name (run-graph r))))))
      (gen "        }")))
  (gen (format "        std::unique_ptr<~a> app(new ~a(context, ~a, std::move(inputs)));" name name
               (if (or single? (null? runs)) "graph" "graphs")))
  (for ([r (in-list runs)])
    (define g (run-graph r))
    (define gname (graph-name g))
    (define members
      (append
       (for/list ([b (in-list (graph-run-bindings r))])
         (match-define (cons input e) b)
         (if (g-parameter? (findf (λ (i) (equal? (graph-item-name i) input)) (graph-items g)))
             (format ".~a = app->~a()" input (run-name r (format "parameter_~a" input)))
             (format ".~a = [self = app.get()](const zgal::FrameInfo&) { return self->~a(); }" input (run-name r (format "input_~a" input)))))
       (for/list ([m (in-list (passthrough r))])
         (define n (graph-item-name (car m)))
         (format ".~a = std::move(app->inputs_.~a)" n n))))
    (emit! w (format "        TRY_ASSIGN(auto ids~a, ~a::build(context, *app->~a, ~a::Inputs{" (run-index r) gname (graph-member r) gname)
           #:at (graph-run-loc r))
    ;; Designated initializers in the graph's declaration order
    (define order (map (λ (m) (graph-item-name (car m))) (graph-input-members g)))
    (for ([member (in-list (sort members < #:key (λ (m) (index-of order (cadr (regexp-match #px"^\\.([A-Za-z0-9_]+)" m))))))])
      (emit! w (format "            ~a," member) #:at (graph-run-loc r)))
    (emit! w "        }));" #:at (graph-run-loc r))
    (gen (format "        app->~a.emplace(std::move(ids~a));" (ids-member r) (run-index r))))
  (gen "        return app;")
  (gen "    }")
  (gen "")
  (gen "    const State& state() const { return state_; }")
  (gen "    // Replaces the state, e.g. for tests driving the app")
  (gen "    void setState(State state) { state_ = std::move(state); }")
  (for ([r (in-list runs)])
    (define gname (graph-name (run-graph r)))
    (gen (format "    // Graph ~a's ids: its images, buffers and nodes" gname))
    (gen (format "    const ~a& ~a() const { return *~a; }" gname (if single? "ids" (format "~aIds" (lower-first gname))) (ids-member r))))
  (gen (if (or single? (null? runs))
           "    RenderGraph& renderGraph() { return *active_; }"
           "    // The graph to draw this frame (chosen by beginFrame)\n    RenderGraph& renderGraph() { return *active_; }"))
  (gen "    VulkanContext& context() { return *context_; }")
  (gen "    // Whether a handler asked to quit (quit!)")
  (gen "    bool quitRequested() const { return quit_; }")
  (gen "")

  ;; handle
  (gen "    // Runs the handler for event, if the app has one")
  (gen "    void handle(const zgal::Event& event) {")
  (define by-kind (group-by (λ (h) (event-kind (handler-event h)))
                            (filter (λ (h) (not (memq (event-kind (handler-event h)) '(frame read)))) (app-handlers a))))
  (for ([group (in-list by-kind)])
    (define kind (event-kind (handler-event (first group))))
    (gen (format "        if (const auto* e = std::get_if<zgal::~a>(&event)) {" (event-struct kind)))
    (case kind
      [(key-pressed key-released)
       (gen "            switch (e->key) {")
       (for ([h (in-list group)])
         (for ([k (in-list (event-keys (handler-event h)))])
           (emit! w (format "                case ~a:" (key-enumerator k)) #:at (handler-loc h)))
         (emit! w (format "                    state_ = ~a(state_);" (handler-name (index-of-handler h))) #:at (handler-loc h))
         (gen "                    break;"))
       (gen "                default:")
       (gen "                    break;")
       (gen "            }")]
      [(mouse-pressed mouse-released)
       (for ([h (in-list group)])
         (emit! w (format "            if (e->button == ~a) {" (button-enumerator (event-button (handler-event h)))) #:at (handler-loc h))
         (emit! w (format "                state_ = ~a(state_, e->cursor);" (handler-name (index-of-handler h))) #:at (handler-loc h))
         (gen "            }"))]
      [(cursor-moved)
       (emit! w (format "            state_ = ~a(state_, e->cursor);" (handler-name (index-of-handler (first group)))) #:at (handler-loc (first group)))]
      [(scrolled)
       (emit! w (format "            state_ = ~a(state_, e->offset);" (handler-name (index-of-handler (first group)))) #:at (handler-loc (first group)))])
    (gen "        }"))
  (when (null? by-kind)
    (gen "        (void)event;"))
  (gen "    }")
  (gen "")

  ;; beginFrame
  (gen (format "    // Each frame, before drawing: ~athe frame handler, then which graph is drawn and which of its nodes are enabled"
               (if (null? read-handlers) "" "the handlers of readbacks' finished copies, ")))
  (gen "    Result<void> beginFrame() {")
  (for ([h (in-list read-handlers)])
    (match-define (list g-name node-name element) (event-readback (handler-event h)))
    (define r (run-of-graph g-name))
    (define conditional? (g-node-when (node-of r node-name)))
    (define id (format "~a->nodes.~a" (ids-member r) node-name))
    (define t (spell-type element 'cpp))
    (define indent (if conditional? "            " "        "))
    (when conditional?
      (emit! w (format "        if (~a) {" id) #:at (handler-loc h)))
    (emit! w (format "~a{" indent) #:at (handler-loc h))
    (emit! w (format "~a    TRY_ASSIGN(auto copied, ~a->nodeAs<BufferReadbackNode>(~a).takeFinished<~a>().withContext(~a));" indent
                     (graph-member r) (if conditional? (string-append "*" id) id) t (cpp-string (format "reading back ~a" node-name)))
           #:at (handler-loc h))
    (emit! w (format "~a    if (copied) {" indent) #:at (handler-loc h))
    (emit! w (format "~a        state_ = ~a(state_, zgal::Data<~a>(std::move(*copied)));" indent (handler-name (index-of-handler h)) t)
           #:at (handler-loc h))
    (emit! w (format "~a    }" indent) #:at (handler-loc h))
    (emit! w (format "~a}" indent) #:at (handler-loc h))
    (when conditional?
      (emit! w "        }" #:at (handler-loc h))))
  (when frame-handler
    (emit! w (format "        state_ = ~a(state_, inputs_.timestep.next());" (handler-name (index-of-handler frame-handler)))
           #:at (handler-loc frame-handler)))
  ;; The active graph: the first whose condition holds
  (unless (or single? (null? runs))
    (for ([r (in-list runs)] [i (in-naturals)])
      (define keyword (if (zero? i) "if" "} else if"))
      (cond
        [(graph-run-when r)
         (emit! w (format "        ~a (~a()) {" keyword (run-name r "active")) #:at (graph-run-loc r))]
        [else (emit! w "        } else {" #:at (graph-run-loc r))])
      (emit! w (format "            active_ = ~a;" (graph-member r)) #:at (graph-run-loc r)))
    (gen "        }"))
  (for ([r (in-list runs)])
    (define enables (graph-run-enables r))
    (unless (null? enables)
      (define indent (if single? "        " "            "))
      (unless single?
        (emit! w (format "        if (active_ == ~a) {" (graph-member r)) #:at (graph-run-loc r)))
      (for ([en (in-list enables)])
        (define node (node-of r (car en)))
        (define ids (ids-member r))
        (emit! w (cond
                   ;; One per iteration of its loop
                   [(pair? (g-node-loops node))
                    (format "~afor (const NodeId node : ~a->nodes.~a) { ~a->setEnabled(node, ~a()); }" indent ids (car en) (graph-member r) (run-name r (format "enabled_~a" (car en))))]
                   [(g-node-when node)
                    (format "~aif (~a->nodes.~a) { ~a->setEnabled(*~a->nodes.~a, ~a()); }" indent ids (car en) (graph-member r) ids (car en) (run-name r (format "enabled_~a" (car en))))]
                   [else
                    (format "~a~a->setEnabled(~a->nodes.~a, ~a());" indent (graph-member r) ids (car en) (run-name r (format "enabled_~a" (car en))))])
               #:at (texpr-loc (cdr en))))
      (unless single?
        (gen "        }"))))
  (gen "        return {};")
  (gen "    }")
  (gen "")

  ;; The bodies
  (gen "private:")
  (cond
    [(or single? (null? runs))
     (gen (format "    ~a(VulkanContext& context, RenderGraph& graph, Inputs inputs)" name))
     (gen (format "    : context_(&context), ~aactive_(&graph), inputs_(std::move(inputs)), state_(initialState()) {}"
                  (if single? "graph0_(&graph), " "")))]
    [else
     (gen (format "    ~a(VulkanContext& context, Graphs graphs, Inputs inputs)" name))
     (gen (format "    : context_(&context), ~a, active_(graphs.~a), inputs_(std::move(inputs)), state_(initialState()) {}"
                  (string-join (for/list ([r (in-list runs)])
                                 (format "~a(graphs.~a)" (graph-member r) (lower-first (graph-name (run-graph r)))))
                               ", ")
                  ;; Until beginFrame chooses: the last, which runs when no other's condition holds
                  (lower-first (graph-name (run-graph (last runs))))))])
  (gen "")
  (emit-initial-state! w a)
  ;; The extern functions, as bodies call them
  (for ([f (in-list (app-externs a))])
    (define params (function-decl-params f))
    (emit! w (format "    ~a ~a(~a) { ~ainputs_.~a(~a); }" (spell-type (function-decl-result f) 'cpp) (function-decl-name f)
                     (string-join (for/list ([p (in-list params)]) (format "~a ~a" (extern-parameter-type (cdr p)) (car p))) ", ")
                     (if (void-t? (function-decl-result f)) "" "return ")
                     (function-decl-name f)
                     (string-join (map car params) ", "))
           #:at (function-decl-loc f)))
  (for ([h (in-list (app-handlers a))])
    (define binding (event-binding (handler-event h)))
    (define binding-type
      (case (event-kind (handler-event h))
        [(frame) "float"]
        [(read) (format "const ~a&" (spell-type (data-t (third (event-readback (handler-event h)))) 'cpp))]
        [else "glm::vec2"]))
    (emit-body-function! w (format "State ~a([[maybe_unused]] const State& state~a)" (handler-name (index-of-handler h))
                                   (if binding (format ", [[maybe_unused]] ~a ~a" binding-type binding) ""))
                         (handler-body h) (handler-loc h)))
  (for ([r (in-list runs)])
    (define g (run-graph r))
    (for ([b (in-list (graph-run-bindings r))])
      (match-define (cons input e) b)
      (define item (findf (λ (i) (equal? (graph-item-name i) input)) (graph-items g)))
      (if (g-parameter? item)
          (emit-body-function! w (format "~a ~a()" (value-type-cpp (texpr-type e)) (run-name r (format "parameter_~a" input))) e (texpr-loc e))
          (emit-body-function! w (format "~a ~a()" (value-type-cpp (texpr-type e)) (run-name r (format "input_~a" input))) e (texpr-loc e)
                               #:state "[[maybe_unused]] const State& state = state_;")))
    (for ([en (in-list (graph-run-enables r))])
      (emit-body-function! w (format "bool ~a()" (run-name r (format "enabled_~a" (car en)))) (cdr en) (texpr-loc (cdr en))
                           #:state "[[maybe_unused]] const State& state = state_;"))
    (when (graph-run-when r)
      (emit-body-function! w (format "bool ~a()" (run-name r "active")) (graph-run-when r) (texpr-loc (graph-run-when r))
                           #:state "[[maybe_unused]] const State& state = state_;")))
  (gen "    VulkanContext* context_;")
  (for ([r (in-list runs)])
    (gen (format "    RenderGraph* ~a;" (graph-member r))))
  (gen "    RenderGraph* active_;")
  (gen "    Inputs inputs_;")
  (gen "    State state_;")
  (gen "    bool quit_ = false;")
  (for ([r (in-list runs)])
    (gen (format "    std::optional<~a> ~a;" (graph-name (run-graph r)) (ids-member r))))
  (emit! w "};" #:at (app-loc a)))

;; How extern functions take a parameter: data by reference, as it's shared anyway
(define (extern-parameter-type t)
  (if (data-t? t) (format "const ~a&" (spell-type t 'cpp)) (spell-type t 'cpp)))

;; initialState(): the state's fields' initial values, from the parameters
(define (emit-initial-state! w a)
  (define state (app-state a))
  (define n 0)
  (define (fresh base)
    (set! n (add1 n))
    (format "zgal~a~a" (string-titlecase base) n))
  (emit! w "    State initialState() const {" #:at (struct-decl-loc state))
  (define field-values
    (for/list ([f (in-list (struct-decl-fields state))] [e (in-list (app-initial a))])
      (define-values (stmts value) (lower e fresh))
      (emit-statements! w stmts 'cpp "        ")
      (format ".~a = ~a" (field-name f) (print-expression value 'cpp))))
  (emit! w (format "        return State{~a};" (string-join field-values ", ")) #:at (struct-decl-loc state))
  (emit! w "    }" #:at (struct-decl-loc state)))

(define (value-type-cpp t)
  (spell-type t 'cpp))

;; A C++ member declaration of type t
(define (declaration-cpp t name)
  (format "~a ~a" (spell-type t 'cpp) name))
