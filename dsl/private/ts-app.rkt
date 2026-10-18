#lang racket/base
;; The TypeScript backend for apps: a class per app holding its state, running its handlers on events
;; and each frame, and building its graphs with inputs computed from the state, as the C++ backend's
;; (cpp-app.rkt) does. Uses the runtime in web/zgal (app.ts for events, Timestep and RunMode).

(require racket/list
         racket/match
         racket/string
         "app.rkt"
         "ast.rkt"
         "body.rkt"
         "errors.rkt"
         "ts-body.rkt"
         "ts-common.rkt")

(provide emit-ts-app)

(define (lower-first s) (string-append (string-downcase (substring s 0 1)) (substring s 1)))

;; Whether an app computes a graph's input once, when it's created: parameters and paths
(define (computed-once? item)
  (or (g-parameter? item) (and (g-input? item) (eq? (g-input-kind item) 'path))))

;; Whether an item is one of a graph's inputs, which C++ and TypeScript give its build
(define (graph-input? item)
  (or (g-parameter? item) (g-input? item) (g-import? item) (g-import-images? item) (g-import-buffer? item)))

;; How bodies read the app's values
(define (host-value name)
  (match name
    ["framesInFlight" "this.context.framesInFlight"]
    ["surfaceSize" "this.active.surfaceExtent().map(Math.fround)"]
    ["frameNumber" "this.frameNumber"]
    ["headless" "this.inputs.mode.headless"]
    ["capture" "this.inputs.mode.capture"]
    ["frameLimit" "this.inputs.mode.frameLimit"]
    [_ (format "this.inputs.~a" name)]))

(define (emit-ts-app prog a)
  (define name (app-name a))
  (define out '())
  (define (line! fmt . args) (set! out (cons (apply format fmt args) out)))
  (define (lines! ls) (for ([l (in-list ls)]) (line! "~a" l)))
  (define state (app-state a))
  (define state-type (format "~aState" name))
  (define runs (app-runs a))
  (define single? (= (length runs) 1))
  (define handlers (app-handlers a))
  (define (kind-of h) (event-kind (handler-event h)))
  (define frame-handler (findf (λ (h) (eq? (kind-of h) 'frame)) handlers))
  (define read-handlers (filter (λ (h) (eq? (kind-of h) 'read)) handlers))
  (define (run-graph r) (graph-run-graph r))
  (define (run-index r) (index-of runs r))
  (define (graph-member r) (format "this.graph~a" (run-index r)))
  (define (ids-member r) (format "this.ids~a" (run-index r)))
  (define (run-name r base) (if single? base (format "~a_~a" base (graph-name (run-graph r)))))
  (define (run-of-graph g-name) (findf (λ (r) (equal? (graph-name (run-graph r)) g-name)) runs))
  (define (node-of r node-name) (findf (λ (n) (equal? (g-node-name n) node-name)) (graph-nodes (run-graph r))))
  (define (item-of g input) (findf (λ (i) (equal? (graph-item-name i) input)) (graph-items g)))
  (define (handler-name h) (format "on~a" (index-of handlers h)))
  (define (passthrough r)
    (for/list ([item (in-list (graph-items (run-graph r)))]
               #:when (and (graph-input? item) (not (assoc (graph-item-name item) (graph-run-bindings r)))))
      item))
  (define extern-names (map function-decl-name (app-externs a)))
  (define (statement text) (if (regexp-match? #rx"}$" text) text (string-append text ";")))
  ;; A node's ids: one, one per iteration of its loop, or one if its #:when held
  (define (for-node r n each)
    (define ids (format "~a.nodes.~a" (ids-member r) (g-node-name n)))
    (cond
      [(pair? (g-node-loops n)) (format "for (const node of ~a) ~a" ids (statement (each "node")))]
      [(g-node-when n) (format "if (~a !== undefined) ~a" ids (statement (each ids)))]
      [else (statement (each ids))]))

  (for ([h (in-list read-handlers)] #:when (read-event-image? (event-readback (handler-event h))))
    (zgal-error 'not-on-web (handler-loc h) "apps on the web can't read images' copies yet (~a)" (read-event-node (event-readback (handler-event h)))))

  (parameterize ([ts-struct-name (let ([outer (ts-struct-name)]) (λ (d) (if (eq? d state) state-type (outer d))))]
                 [ts-host-value host-value]
                 [ts-call-target (λ (f) (if (member f extern-names) (format "this.inputs.~a" f) (ts-name f)))])
    (line! "")
    (line! "// app ~a: its state, the handlers computing the next one, and ~a" name
           (match runs
             ['() "no graph"]
             [(list r) (format "graph ~a" (graph-name (run-graph r)))]
             [_ (format "graphs ~a, one active each frame" (string-join (map (λ (r) (graph-name (run-graph r))) runs) ", "))]))
    (line! "export interface ~a {" state-type)
    (for ([f (in-list (struct-decl-fields state))])
      (line! "  ~a: ~a;" (field-name f) (ts-value-type (field-type f))))
    (line! "}")
    (line! "")
    (line! "// What the page or Node gives it: the parameters, the clock, and graph inputs the app doesn't compute")
    (line! "export interface ~aInputs {" name)
    (for ([p (in-list (app-parameters a))])
      (line! "  ~a: ~a;" (field-name p) (ts-value-type (field-type p))))
    (when frame-handler
      (line! "  // How far the frame handler's clock advances a frame")
      (line! "  timestep: zgal.Timestep;"))
    (when (app-run-mode? a)
      (line! "  // How the app is run: headless, capture and frameLimit")
      (line! "  mode: zgal.RunMode;"))
    (for ([f (in-list (app-externs a))])
      (line! "  ~a: (~a) => ~a;" (function-decl-name f)
             (string-join (for/list ([p (in-list (function-decl-params f))]) (format "~a: ~a" (ts-name (car p)) (ts-value-type (cdr p)))) ", ")
             (ts-value-type (function-decl-result f))))
    (for* ([r (in-list runs)] [item (in-list (passthrough r))])
      (line! "  ~a: ~aInputs['~a'];" (graph-item-name item) (graph-name (run-graph r)) (graph-item-name item)))
    (line! "}")
    (unless (or single? (null? runs))
      (line! "")
      (line! "// The render graphs it adds its graphs to, one per graph")
      (line! "export interface ~aGraphs {" name)
      (for ([r (in-list runs)])
        (line! "  ~a: zgal.RenderGraph;" (lower-first (graph-name (run-graph r)))))
      (line! "}"))
    (line! "")
    (line! "export class ~a {" name)
    (line! "  readonly context: zgal.Context;")
    (line! "  private readonly inputs: ~aInputs;" name)
    (for ([r (in-list runs)])
      (line! "  private readonly graph~a: zgal.RenderGraph;" (run-index r))
      (line! "  private ids~a!: ~a;" (run-index r) (graph-name (run-graph r))))
    (line! "  private active: zgal.RenderGraph;")
    (line! "  // The frame being drawn (frameNumber; 0 until the first), and the frames begun")
    (line! "  private frameNumber = 0;")
    (line! "  private framesBegun = 0;")
    (line! "  private current: ~a;" state-type)
    (line! "  private quit = false;")
    (line! "  private resize: [number, number] | null = null;")
    (line! "")
    (define graphs-parameter
      (cond [(or single? (null? runs)) "graph: zgal.RenderGraph"] [else (format "graphs: ~aGraphs" name)]))
    (line! "  private constructor(context: zgal.Context, ~a, inputs: ~aInputs) {" graphs-parameter name)
    (line! "    this.context = context;")
    (line! "    this.inputs = inputs;")
    (cond
      [(null? runs) (line! "    this.active = graph;")]
      [single? (line! "    this.graph0 = graph;") (line! "    this.active = graph;")]
      [else
       (for ([r (in-list runs)])
         (line! "    this.graph~a = graphs.~a;" (run-index r) (lower-first (graph-name (run-graph r)))))
       ;; Until beginFrame chooses: the last, which runs when no other's condition holds
       (line! "    this.active = graphs.~a;" (lower-first (graph-name (run-graph (last runs)))))])
    (line! "    this.current = this.initialState();")
    (line! "  }")
    (line! "")
    (line! "  // The app, with its initial state~a. The graphs' callbacks use the app, so it must outlive their use."
           (cond [(null? runs) ""] [single? ", adding its graph to `graph`"] [else ", adding each graph to its render graph"]))
    (line! "  static async create(context: zgal.Context, ~a, inputs: ~aInputs): Promise<~a> {" graphs-parameter name name)
    (line! "    const app = new ~a(context, ~a, inputs);" name (if (or single? (null? runs)) "graph" "graphs"))
    (for ([r (in-list runs)])
      (define g (run-graph r))
      (define computed
        (for/list ([b (in-list (graph-run-bindings r))])
          (define input (car b))
          (cons input
                (if (computed-once? (item-of g input))
                    (format "app.~a()" (run-name r (format "parameter_~a" input)))
                    (format "() => app.~a()" (run-name r (format "input_~a" input)))))))
      (define given (for/list ([item (in-list (passthrough r))]) (cons (graph-item-name item) (format "inputs.~a" (graph-item-name item)))))
      (define order (map graph-item-name (filter graph-input? (graph-items g))))
      (line! "    app.ids~a = await ~a.build(context, app.graph~a, {" (run-index r) (graph-name g) (run-index r))
      (for ([m (in-list (sort (append computed given) < #:key (λ (m) (index-of order (car m)))))])
        (line! "      ~a: ~a," (car m) (cdr m)))
      (line! "    });"))
    (line! "    return app;")
    (line! "  }")
    (line! "")
    (line! "  get state(): ~a {" state-type)
    (line! "    return this.current;")
    (line! "  }")
    (line! "")
    (line! "  // Replaces the state, e.g. for tests driving the app")
    (line! "  setState(state: ~a): void {" state-type)
    (line! "    this.current = state;")
    (line! "  }")
    (for ([r (in-list runs)])
      (define gname (graph-name (run-graph r)))
      (line! "")
      (line! "  // Graph ~a's ids: its images, buffers and nodes" gname)
      (line! "  get ~a(): ~a {" (if single? "ids" (format "~aIds" (lower-first gname))) gname)
      (line! "    return this.ids~a;" (run-index r))
      (line! "  }"))
    (line! "")
    (line! "  // The graph to draw this frame~a" (if (or single? (null? runs)) "" " (chosen by beginFrame)"))
    (line! "  renderGraph(): zgal.RenderGraph {")
    (line! "    return this.active;")
    (line! "  }")
    (line! "")
    (line! "  // Whether a handler asked to quit (quit!)")
    (line! "  quitRequested(): boolean {")
    (line! "    return this.quit;")
    (line! "  }")
    (line! "")
    (line! "  // The size a handler last asked for (resize!), once: the canvas's, or headless, the graphs' surface extent")
    (line! "  takeResize(): [number, number] | null {")
    (line! "    const size = this.resize;")
    (line! "    this.resize = null;")
    (line! "    return size;")
    (line! "  }")
    (line! "")
    (line! "  // Sets the surface extent of every graph the app runs, headless")
    (line! "  setSurfaceExtent(extent: [number, number]): void {")
    (if (null? runs)
        (line! "    this.active.setSurfaceExtent(extent);")
        (for ([r (in-list runs)]) (line! "    ~a.setSurfaceExtent(extent);" (graph-member r))))
    (line! "  }")
    (line! "")

    ;; handle
    (line! "  // Runs the handler for event, if the app has one")
    (line! "  handle(event: zgal.Event): void {")
    (define by-kind (group-by kind-of (filter (λ (h) (not (memq (kind-of h) '(frame read)))) handlers)))
    (if (null? by-kind)
        (line! "    void event;")
        (line! "    switch (event.kind) {"))
    (for ([group (in-list by-kind)])
      (define kind (kind-of (first group)))
      (line! "      case '~a':" kind)
      (case kind
        [(key-pressed key-released)
         (line! "        switch (event.key) {")
         (for ([h (in-list group)])
           (for ([k (in-list (event-keys (handler-event h)))])
             (line! "          case '~a':" (cdr (assoc k key-names))))
           (line! "            this.current = this.~a(this.current);" (handler-name h))
           (line! "            break;"))
         (line! "        }")]
        [(mouse-pressed mouse-released)
         (for ([h (in-list group)])
           (line! "        if (event.button === '~a') this.current = this.~a(this.current, event.cursor);"
                  (event-button (handler-event h)) (handler-name h)))]
        [(cursor-moved) (line! "        this.current = this.~a(this.current, event.cursor);" (handler-name (first group)))]
        [(scrolled) (line! "        this.current = this.~a(this.current, event.offset);" (handler-name (first group)))])
      (line! "        break;"))
    (unless (null? by-kind)
      (line! "    }"))
    (line! "  }")
    (line! "")

    ;; beginFrame
    (line! "  // Each frame, before drawing: ~athe frame handler, then which graph is drawn and which of its nodes are enabled"
           (if (null? read-handlers) "" "the handlers of readbacks' finished copies, "))
    (line! "  beginFrame(): void {")
    (line! "    this.frameNumber = this.framesBegun++;")
    (for ([h (in-list read-handlers)])
      (match-define (read-event g-name node-name element looped? _ _ _) (event-readback (handler-event h)))
      (define r (run-of-graph g-name))
      (define n (node-of r node-name))
      (define (take id) (format "~a.node<zgal.BufferReadbackNode>(~a).takeFinished<~a>()" (graph-member r) id (ts-value-type element)))
      (line! "    ~a"
             (for-node r n (λ (id)
                             (format "{ const copied = ~a; if (copied) this.current = this.~a(this.current, copied~a); }"
                                     (take id) (handler-name h)
                                     (if looped? (format ", ~a.nodes.~a.indexOf(~a)" (ids-member r) node-name id) ""))))))
    (when frame-handler
      (line! "    this.current = this.~a(this.current, this.inputs.timestep.next());" (handler-name frame-handler)))
    (unless (or single? (null? runs))
      (for ([r (in-list runs)] [i (in-naturals)])
        (define keyword (if (zero? i) "if" "} else if"))
        (if (graph-run-when r)
            (line! "    ~a (this.~a()) {" keyword (run-name r "active"))
            (line! "    } else {"))
        (line! "      this.active = ~a;" (graph-member r)))
      (line! "    }"))
    (for ([r (in-list runs)])
      (define enables (graph-run-enables r))
      (unless (null? enables)
        (define indent (if single? "    " "      "))
        (unless single? (line! "    if (this.active === ~a) {" (graph-member r)))
        (for ([en (in-list enables)])
          (line! "~a~a" indent
                 (for-node r (node-of r (car en))
                           (λ (id) (format "~a.setEnabled(~a, this.~a())" (graph-member r) id (run-name r (format "enabled_~a" (car en))))))))
        (unless single? (line! "    }"))))
    (line! "  }")
    (line! "")

    ;; The bodies
    (lines! (ts-body-function (format "private initialState(): ~a" state-type)
                              (t-construct (app-initial a) (struct-t state) (app-loc a)) "  "))
    (define run-once-nodes
      (remove-duplicates
       (for*/list ([h (in-list handlers)]
                   [e (in-list (let walk ([e (handler-body h)]) (cons e (append-map walk (texpr-children e)))))]
                   #:when (and (t-effect? e) (eq? (t-effect-op e) 'run-once)))
         (t-lit-value (first (t-effect-args e))))))
    (for ([node (in-list run-once-nodes)])
      (line! "")
      (line! "  private runOnce_~a(): void {" node)
      (for* ([r (in-list runs)] [n (in-value (node-of r node))] #:when n)
        (line! "    ~a" (for-node r n (λ (id) (format "~a.runOnce(~a)" (graph-member r) id)))))
      (line! "  }"))
    (for ([h (in-list handlers)])
      (define ev (handler-event h))
      (define binding (event-binding ev))
      (define r (event-readback ev))
      (define binding-type
        (case (kind-of h)
          [(frame) "number"]
          [(read) (format "readonly ~a[]" (ts-value-type (read-event-element r)))]
          [else "[number, number]"]))
      (define parameters
        (append (list (format "state: ~a" state-type))
                (if (memq (kind-of h) '(key-pressed key-released)) '()
                    (list (format "~a: ~a" (if binding (ts-name binding) "_value") binding-type)))
                (if (and r (read-event-looped? r)) (list (format "~a: number" (if (read-event-index r) (ts-name (read-event-index r)) "_index"))) '())))
      (line! "")
      (lines! (ts-body-function (format "private ~a(~a): ~a" (handler-name h) (string-join parameters ", ") state-type)
                                (handler-body h) "  ")))
    (define state-line (list "const state = this.current;"))
    (for ([r (in-list runs)])
      (define g (run-graph r))
      (for ([b (in-list (graph-run-bindings r))])
        (match-define (cons input e) b)
        (define once? (computed-once? (item-of g input)))
        (line! "")
        (lines! (ts-body-function (format "private ~a(): ~a" (run-name r (format "~a_~a" (if once? "parameter" "input") input))
                                          (ts-value-type (texpr-type e)))
                                  e "  " #:first-lines (if once? '() state-line))))
      (for ([en (in-list (graph-run-enables r))])
        (line! "")
        (lines! (ts-body-function (format "private ~a(): boolean" (run-name r (format "enabled_~a" (car en)))) (cdr en) "  "
                                  #:first-lines state-line)))
      (when (graph-run-when r)
        (line! "")
        (lines! (ts-body-function (format "private ~a(): boolean" (run-name r "active")) (graph-run-when r) "  "
                                  #:first-lines state-line))))
    (line! "}"))
  (reverse out))
