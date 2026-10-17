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

(define (emit-app! w prog a)
  (define name (app-name a))
  (define (gen text) (emit! w text))
  (define state (app-state a))
  (define run (app-run a))
  (define g (and run (graph-run-graph run)))
  (define frame-handler (for/first ([h (in-list (app-handlers a))] #:when (eq? (event-kind (handler-event h)) 'frame)) h))
  (define passthrough
    (if run
        (for/list ([m (in-list (graph-input-members g))]
                   #:unless (assoc (graph-item-name (car m)) (graph-run-bindings run)))
          m)
        '()))
  (define (handler-name i) (format "on~a" i))

  (gen "")
  (gen (format "// app ~a: its state, the handlers computing the next one, and ~a" name
               (if run (format "graph ~a" (graph-name g)) "no graph")))
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
  (for ([m (in-list passthrough)])
    (emit! w (string-append "        " (cdr m)) #:at (item-loc (car m))))
  (gen "    };")
  (gen "")

  ;; create
  (gen (format "    // The app, with its initial state~a. The graph's callbacks"
               (if run ", adding its graph to `graph`" "")))
  (gen "    // use the app, so it must outlive the graph's use.")
  (gen (format "    static Result<std::unique_ptr<~a>> create(VulkanContext& context, RenderGraph& graph, Inputs inputs) {" name))
  (when frame-handler
    (gen "        if (!inputs.timestep.given()) {")
    (gen (format "            contractViolation(~a);" (cpp-string (format "~a's timestep isn't given: zgal::Timestep::realTime or fixed" name))))
    (gen "        }"))
  (gen (format "        std::unique_ptr<~a> app(new ~a(context, graph, std::move(inputs)));" name name))
  (when run
    (define gname (graph-name g))
    (define members
      (append
       (for/list ([b (in-list (graph-run-bindings run))])
         (match-define (cons input e) b)
         (if (g-parameter? (findf (λ (i) (equal? (graph-item-name i) input)) (graph-items g)))
             (format ".~a = app->parameter_~a()" input input)
             (format ".~a = [self = app.get()](const zgal::FrameInfo&) { return self->input_~a(); }" input input)))
       (for/list ([m (in-list passthrough)])
         (define n (graph-item-name (car m)))
         (format ".~a = std::move(app->inputs_.~a)" n n))))
    (emit! w (format "        TRY_ASSIGN(auto ids, ~a::build(context, graph, ~a::Inputs{" gname gname) #:at (graph-run-loc run))
    ;; Designated initializers in the graph's declaration order
    (define order (map (λ (m) (graph-item-name (car m))) (graph-input-members g)))
    (for ([member (in-list (sort members < #:key (λ (m) (index-of order (cadr (regexp-match #px"^\\.([A-Za-z0-9_]+)" m))))))])
      (emit! w (format "            ~a," member) #:at (graph-run-loc run)))
    (emit! w "        }));" #:at (graph-run-loc run))
    (gen "        app->ids_.emplace(std::move(ids));"))
  (gen "        return app;")
  (gen "    }")
  (gen "")
  (gen "    const State& state() const { return state_; }")
  (gen "    // Replaces the state, e.g. for tests driving the app")
  (gen "    void setState(State state) { state_ = std::move(state); }")
  (when run
    (gen (format "    // The graph's ids: its images, buffers and nodes"))
    (gen (format "    const ~a& ids() const { return *ids_; }" (graph-name g))))
  (gen "    RenderGraph& renderGraph() { return *graph_; }")
  (gen "    VulkanContext& context() { return *context_; }")
  (gen "    // Whether a handler asked to quit (quit!)")
  (gen "    bool quitRequested() const { return quit_; }")
  (gen "")

  ;; handle
  (gen "    // Runs the handler for event, if the app has one")
  (gen "    void handle(const zgal::Event& event) {")
  (define by-kind (group-by (λ (h) (event-kind (handler-event h)))
                            (filter (λ (h) (not (eq? (event-kind (handler-event h)) 'frame))) (app-handlers a))))
  (define indexed (for/list ([h (in-list (app-handlers a))] [i (in-naturals)]) (cons h i)))
  (define (index-of-handler h) (cdr (assq h indexed)))
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
  (gen "    // Each frame, before drawing: the frame handler, then whether nodes are enabled")
  (gen "    void beginFrame() {")
  (when frame-handler
    (emit! w (format "        state_ = ~a(state_, inputs_.timestep.next());" (handler-name (index-of-handler frame-handler)))
           #:at (handler-loc frame-handler)))
  (when run
    (for ([en (in-list (graph-run-enables run))])
      (define node (for/first ([n (in-list (graph-nodes g))] #:when (equal? (g-node-name n) (car en))) n))
      (emit! w (if (g-node-when node)
                   (format "        if (ids_->nodes.~a) { graph_->setEnabled(*ids_->nodes.~a, enabled_~a()); }" (car en) (car en) (car en))
                   (format "        graph_->setEnabled(ids_->nodes.~a, enabled_~a());" (car en) (car en)))
             #:at (texpr-loc (cdr en)))))
  (gen "    }")
  (gen "")

  ;; The bodies
  (gen "private:")
  (gen (format "    ~a(VulkanContext& context, RenderGraph& graph, Inputs inputs)" name))
  (gen "    : context_(&context), graph_(&graph), inputs_(std::move(inputs)), state_(initialState()) {}")
  (gen "")
  (emit-initial-state! w a)
  (for ([h (in-list (app-handlers a))])
    (define binding (event-binding (handler-event h)))
    (define binding-type (if (eq? (event-kind (handler-event h)) 'frame) "float" "glm::vec2"))
    (emit-body-function! w (format "State ~a([[maybe_unused]] const State& state~a)" (handler-name (index-of-handler h))
                                   (if binding (format ", [[maybe_unused]] ~a ~a" binding-type binding) ""))
                         (handler-body h) (handler-loc h)))
  (when run
    (for ([b (in-list (graph-run-bindings run))])
      (match-define (cons input e) b)
      (define item (findf (λ (i) (equal? (graph-item-name i) input)) (graph-items g)))
      (if (g-parameter? item)
          (emit-body-function! w (format "~a parameter_~a()" (value-type-cpp (texpr-type e)) input) e (texpr-loc e))
          (emit-body-function! w (format "~a input_~a()" (value-type-cpp (texpr-type e)) input) e (texpr-loc e)
                               #:state "[[maybe_unused]] const State& state = state_;")))
    (for ([en (in-list (graph-run-enables run))])
      (emit-body-function! w (format "bool enabled_~a()" (car en)) (cdr en) (texpr-loc (cdr en))
                           #:state "[[maybe_unused]] const State& state = state_;")))
  (gen "    VulkanContext* context_;")
  (gen "    RenderGraph* graph_;")
  (gen "    Inputs inputs_;")
  (gen "    State state_;")
  (gen "    bool quit_ = false;")
  (when run
    (gen (format "    std::optional<~a> ids_;" (graph-name g))))
  (emit! w "};" #:at (app-loc a)))

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
