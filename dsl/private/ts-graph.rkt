#lang racket/base
;; The TypeScript backend for graphs: a class per graph, whose static build() adds the graph's
;; resources, materials and nodes to a RenderGraph (web/zgal/graph.ts), like the C++ struct's.

(require racket/list
         racket/match
         racket/string
         "ast.rkt"
         "builtins.rkt"
         "errors.rkt"
         "graph.rkt"
         "ts-common.rkt")

(provide emit-ts-graph
         webgpu-format
         ts-graph-expr)

(define (web-error where fmt . args)
  (apply zgal-error 'not-on-web where fmt args))

;; An object literal or type of members, written "{}" without any
(define (ts-object members separator)
  (if (null? members) "{}" (format "{ ~a }" (string-join members separator))))

;; ZGAL's image formats as WebGPU's
(define webgpu-formats
  (hash 'r8 "r8unorm" 'rg8 "rg8unorm" 'rgba8 "rgba8unorm" 'rgba8-srgb "rgba8unorm-srgb" 'bgra8 "bgra8unorm"
        'bgra8-srgb "bgra8unorm-srgb" 'r16f "r16float" 'rg16f "rg16float" 'rgba16f "rgba16float" 'r32f "r32float"
        'rg32f "rg32float" 'rgba32f "rgba32float" 'r32ui "r32uint" 'r32i "r32sint" 'd16 "depth16unorm"
        'd32 "depth32float" 'd24s8 "depth24plus-stencil8" 'd32s8 "depth32float-stencil8"))
(define (webgpu-format f) (hash-ref webgpu-formats f))

;; ---------------------------------------------------------------------------------------------
;; Expressions

(define (resolve t want)
  (if (equal? t literal-type) (or want (scalar-t 'int)) t))

(define (number-text v t)
  (match t
    [(scalar-t 'float)
     (define text (number->string (exact->inexact v)))
     (if (regexp-match? #rx"[.e]" text) text (string-append text ".0"))]
    [_ (format "~a" v)]))

;; The kind math.ts's operations take for t: 'float 'int or 'uint
(define (kind-of t)
  (match t [(scalar-t k) k] [(vector-t k _) k]))

;; e as TypeScript, of type `want` (which integer literals take). Parameters are read from `inputs`,
;; per-frame inputs called with `frame`.
(define (ts-graph-expr e env [want #f] #:graph-name [graph-name ""])
  (define (recur e [w #f]) (ts-graph-expr e env w #:graph-name graph-name))
  (define (type-of e) (expression-type e env))
  (define (cast t text) (format "(~a as ~a)" text (ts-value-type t)))
  (match e
    [(e-literal v _)
     (cond [(boolean? v) (if v "true" "false")]
           [else (number-text v (resolve (if (exact-integer? v) literal-type (scalar-t 'float)) want))])]
    [(e-ref name _)
     (match (entry-kind (hash-ref env name))
       ['parameter (format "inputs.~a" name)]
       ['constant (format "constant_~a" name)]
       ['per-frame (format "inputs.~a(frame)" name)]
       ['loop (format "loop_~a" name)])]
    [(e-field base field _)
     (define base-text (recur base))
     (match (type-of base)
       [(vector-t _ n)
        (define components (swizzle-components field n))
        (define (index c) (for/first ([d (in-string "xyzw")] [i (in-naturals)] #:when (char=? c d)) i))
        (if (= (string-length components) 1)
            (format "~a[~a]" base-text (index (string-ref components 0)))
            (format "((v: number[]) => [~a])(~a)"
                    (string-join (for/list ([c (in-string components)]) (format "v[~a]" (index c))) ", ") base-text))]
       [_ (format "~a.~a" base-text field)])]
    [(e-op op args _)
     (define t (resolve (type-of e) want))
     (case op
       [(if) (format "(~a ? ~a : ~a)" (recur (first args) (scalar-t 'bool)) (recur (second args) t) (recur (third args) t))]
       [(and or) (format "(~a)" (string-join (map (λ (a) (recur a (scalar-t 'bool))) args) (if (eq? op 'and) " && " " || ")))]
       [(not) (format "(!~a)" (recur (first args) (scalar-t 'bool)))]
       [(= < <= > >=)
        (define operand (resolve (unify (type-of (first args)) (type-of (second args))) #f))
        (format "(~a ~a ~a)" (recur (first args) operand) (if (eq? op '=) "===" op) (recur (second args) operand))]
       [(+ - * /)
        (if (= (length args) 1)
            (cast t (format "zgal.math.neg('~a', ~a)" (kind-of t) (recur (first args) t)))
            (for/fold ([acc (recur (first args) t)]) ([a (in-list (rest args))])
              (cast t (format "zgal.math.~a('~a', ~a, ~a)" (case op [(+) "add"] [(-) "sub"] [(*) "mul"] [(/) "div"])
                              (kind-of t) acc (recur a t)))))]
       [(min max) (cast t (format "zgal.math.~a(~a, ~a)" op (recur (first args) t) (recur (second args) t)))]
       [(count)
        (define input (e-ref-name (first args)))
        (if (eq? (entry-kind (hash-ref env input)) 'per-frame-data)
            (format "data_~a(frame).length" input)
            (format "inputs.~a.length" input))]
       [(float) (format "zgal.math.toFloat(~a)" (recur (first args)))]
       [(int) (format "zgal.math.toInt(~a)" (recur (first args)))]
       [(uint) (format "zgal.math.toUint(~a)" (recur (first args)))]
       [(at)
        (define input (e-ref-name (first args)))
        (format "zgal.math.elementOf(inputs.~a, ~a, ~a)" input (recur (second args)) (ts-string (format "~a's data input ~a" graph-name input)))]
       [(width height)
        (define image (e-ref-name (first args)))
        (define i (if (eq? op 'width) 0 1))
        (match (hash-ref env image)
          [(entry 'import _ item) (format "image_~a.size[~a]" image i)]
          [(entry 'image _ item) (format "graph.imageExtent(~a)[~a]" (format "need(image_~a, ~a)" image (ts-string image)) i)])]
       [else
        (define component (scalar-t (vector-t-kind t)))
        (format "[~a]" (string-join (map (λ (a) (recur a component)) args) ", "))])]))

;; ---------------------------------------------------------------------------------------------
;; The graph

(define (emit-ts-graph prog g)
  (define name (graph-name g))
  (define base-env (graph-environment g))
  (define env base-env)
  (define items (graph-items g))
  (define nodes (graph-nodes g))
  (define out '())
  (define (line! fmt . args) (set! out (cons (apply format fmt args) out)))
  (define (of-type pred) (filter pred items))
  (define (expr e want) (ts-graph-expr e env want #:graph-name name))
  (define (uint-expr e) (expr e (scalar-t 'uint)))
  (define (bool-expr e) (expr e (scalar-t 'bool)))
  (define (internal? n) (string-prefix? n "zgal_"))
  (define (looped? n) (pair? (g-node-loops n)))

  ;; What the web can't do yet
  (for ([item (in-list items)])
    (match item
      [(or (g-model _ _ _ l) (g-model-textures _ _ l) (g-model-factors _ _ l) (g-model-draws _ _ _ l) (g-model-bounds _ _ l))
       (web-error l "models aren't loaded on the web yet")]
      [(g-image-file n _ _ l) (web-error l "image files aren't loaded on the web yet (~a)" n)]
      [(g-import-images n _ l) (web-error l "WebGPU has no arrays of bindings, so ~a's images can't be imported for one" n)]
      [(g-input n 'meshes _ l) (web-error l "meshes aren't drawn on the web yet (~a)" n)]
      [(g-sampler n o l)
       (when (eq? (cond [(assq 'address o) => cdr] [else #f]) 'clamp-to-border)
         (web-error l "WebGPU's samplers have no border colour, so sampler ~a can't clamp to one" n))]
      [_ (void)]))
  (for ([n (in-list nodes)])
    (define l (g-node-loc n))
    (match (g-node-kind n)
      ['kernel (web-error l "the compute library's nodes don't run on the web yet (~a)" (g-node-name n))]
      ['draw-meshes (web-error l "meshes aren't drawn on the web yet (~a)" (g-node-name n))]
      ['cpu (unless (and (null? (g-node-reads n)) (null? (g-node-writes n)))
              (web-error l "cpu node ~a maps buffers, which WebGPU's buffers can't be while the GPU uses them" (g-node-name n)))]
      [_ (when (indirect? (g-node-extra n))
           (web-error l "indirect dispatches and draws don't run on the web yet (~a)" (g-node-name n)))]))

  (define parameters (of-type g-parameter?))
  (define inputs (of-type g-input?))
  (define images (of-type g-image?))
  (define buffers (of-type g-buffer?))

  (line! "")
  (line! "// graph ~a: build() adds its resources, materials and nodes to a RenderGraph" name)
  (line! "export interface ~aInputs {" name)
  (for ([item (in-list items)])
    (match item
      [(g-parameter n t _) (line! "  ~a: ~a;" n (ts-value-type t))]
      [(g-input n kind t _)
       (line! "  ~a: ~a;" n
              (match kind
                ['per-frame (format "zgal.FrameSource<~a>" (ts-value-type t))]
                ['data (format "readonly ~a[]" (ts-value-type t))]
                ['per-frame-data (format "zgal.FrameSource<readonly ~a[]>" (ts-value-type t))]
                ['cpu "(frame: zgal.FrameInfo) => void"]
                ['extern (format "(graph: zgal.RenderGraph, ids: ~a~a) => zgal.NodeId[]" name
                                 (string-append* (for/list ([a (in-list t)] [i (in-naturals)]) (format ", a~a: ~a" i (ts-value-type a)))))]
                ['path "string"]))]
      [(g-import n _ _) (line! "  ~a: GPUTexture;" n)]
      [(g-import-buffer n _ _) (line! "  ~a: GPUBuffer;" n)]
      [_ (void)]))
  (line! "}")

  (define (members kind pred name-of conditional?)
    (for/list ([i (in-list items)] #:when (and (pred i) (not (internal? (name-of i)))))
      (format "~a: zgal.~a~a" (name-of i) kind (if (conditional? i) " | undefined" ""))))
  (define image-members
    (append (members "GraphImage" g-import? graph-item-name (λ (_) #f))
            (members "GraphImage" g-image? graph-item-name g-image-when)))
  (define buffer-members
    (append (members "GraphBuffer" (λ (i) (or (g-import-buffer? i) (g-upload? i) (g-buffer-file? i))) graph-item-name (λ (_) #f))
            (members "GraphBuffer" g-buffer? graph-item-name g-buffer-when)))
  (define node-members
    (for/list ([n (in-list nodes)])
      (format "~a: ~a" (g-node-name n)
              (cond [(or (looped? n) (eq? (g-node-kind n) 'extern)) "zgal.NodeId[]"]
                    [(g-node-when n) "zgal.NodeId | undefined"]
                    [else "zgal.NodeId"]))))

  (line! "")
  (line! "export class ~a {" name)
  (line! "  images: ~a = {} as ~a['images'];" (ts-object image-members "; ") name)
  (line! "  buffers: ~a = {} as ~a['buffers'];" (ts-object buffer-members "; ") name)
  (line! "  nodes: ~a = {} as ~a['nodes'];" (ts-object node-members "; ") name)

  ;; Readbacks' results
  (for ([n (in-list nodes)] #:when (eq? (g-node-kind n) 'readback))
    (define subject (g-node-subject n))
    (define id (cond [(looped? n) (format "this.nodes.~a[index]" (g-node-name n))]
                     [(g-node-when n) (format "zgal.need(this.nodes.~a, ~a)" (g-node-name n) (ts-string (g-node-name n)))]
                     [else (format "this.nodes.~a" (g-node-name n))]))
    (define params (if (looped? n) "graph: zgal.RenderGraph, index: number" "graph: zgal.RenderGraph"))
    (cond
      [(memq (entry-kind (hash-ref env subject)) '(image import))
       (line! "")
       (line! "  // The texels readback ~a copied in the most recent frame (waiting for it)" (g-node-name n))
       (line! "  ~aPixels(~a): Promise<Uint8Array> {" (g-node-name n) params)
       (line! "    return graph.node<zgal.ReadbackNode>(~a).pixels();" id)
       (line! "  }")]
      [else
       (define t (ts-value-type (entry-type (hash-ref env subject))))
       (line! "")
       (line! "  // The elements readback ~a copied in the most recent frame (waiting for it)" (g-node-name n))
       (line! "  ~aContents(~a): Promise<~a[]> {" (g-node-name n) params t)
       (line! "    return graph.node<zgal.BufferReadbackNode>(~a).contents<~a>();" id t)
       (line! "  }")]))

  (line! "")
  (line! "  // Adds the graph's resources, materials and nodes to `graph`. Its materials and nodes keep")
  (line! "  // the inputs' functions, so whatever those use must outlive the graph.")
  (line! "  static async build(context: zgal.Context, graph: zgal.RenderGraph, inputs: ~aInputs): Promise<~a> {" name name)
  (line! "    const result = new ~a();" name)
  (line! "    const need = <T>(x: T | undefined, what: string): T => {")
  (line! "      if (x === undefined) throw new Error(`~a: ${what} isn't created (its #:when is false)`);" name)
  (line! "      return x;")
  (line! "    };")
  (line! "    void need;")
  ;; Per-frame data, computed once a frame however many use it
  (for ([i (in-list inputs)] #:when (eq? (g-input-kind i) 'per-frame-data))
    (line! "    const data_~a = zgal.frameCache(inputs.~a);" (g-input-name i) (g-input-name i)))

  (define (resource-ref n)
    (define e (hash-ref env n))
    (define-values (variable conditional?)
      (match (entry-item e)
        [(g-image _ _ _ _ when _) (values (format "image_~a" n) when)]
        [(g-import _ _ _) (values (format "image_~a" n) #f)]
        [(g-buffer _ _ _ _ when _) (values (format "buffer_~a" n) when)]
        [_ (values (format "buffer_~a" n) #f)]))
    (if conditional? (format "need(~a, ~a)" variable (ts-string n)) variable))
  (define (instance-ref prefix n)
    (define conditional?
      (match (entry-item (hash-ref env n))
        [(g-target _ _ when _) when]
        [(g-material _ _ _ _ _ _ when _) when]))
    (if conditional? (format "need(~a_~a, ~a)" prefix n (ts-string n)) (format "~a_~a" prefix n)))
  (define (image-kind? n) (memq (entry-kind (hash-ref env n)) '(image import)))

  (for ([item (in-list items)])
    (match item
      [(g-import n _ _)
       (line! "    const image_~a = graph.importImage(~a, inputs.~a);" n (ts-string n) n)
       (line! "    result.images.~a = image_~a;" n n)]
      [(g-import-buffer n t _)
       (line! "    const buffer_~a = graph.importBuffer(~a, ~a, inputs.~a);" n (ts-string n) (ts-layout-type t) n)
       (line! "    result.buffers.~a = buffer_~a;" n n)]
      [(g-upload n source t _)
       (cond
         [(eq? (entry-kind (hash-ref env source)) 'per-frame-data)
          (line! "    const buffer_~a = graph.createBuffer(~a, ~a, (frame) => data_~a(frame).length);" n (ts-string n) (ts-layout-type t) source)
          (line! "    graph.add(new zgal.DataUploadNode(~a, graph, buffer_~a, data_~a));" (ts-string (format "upload ~a" n)) n source)]
         [else
          (line! "    const buffer_~a = graph.upload(~a, ~a, inputs.~a);" n (ts-string n) (ts-layout-type t) source)])
       (line! "    result.buffers.~a = buffer_~a;" n n)]
      [(g-buffer-file n t sources _)
       (line! "    const buffer_~a = await zgal.loadBufferFile(context, graph, ~a, ~a, [~a]);" n (ts-string n) (ts-layout-type t)
              (string-join (for/list ([s (in-list sources)])
                             (if (asset-source-path s) (ts-string (asset-source-path s)) (format "inputs.~a" (asset-source-input s))))
                           ", "))
       (line! "    result.buffers.~a = buffer_~a;" n n)]
      [(g-constant n e t _)
       (line! "    const constant_~a = ~a;" n (expr e t))]
      [(g-sampler n o _)
       (define (opt k) (cond [(assq k o) => cdr] [else #f]))
       (define filter (if (eq? (opt 'filter) 'nearest) "nearest" "linear"))
       ;; Without a mipmap filter, a nearest sampler's is nearest too: with no mipmaps (#:max-lod 0)
       ;; it filters nothing, and WebGPU counts a linear one as filtering
       (define mipmap (case (opt 'mipmap) [(nearest) "nearest"] [(linear) "linear"] [else filter]))
       (define address (case (opt 'address) [(mirrored-repeat) "mirror-repeat"] [(clamp-to-edge) "clamp-to-edge"] [else "repeat"]))
       (define anisotropic? (and (not (eq? (opt 'anisotropy) 'no)) (equal? filter "linear") (equal? mipmap "linear")))
       (line! "    const sampler_~a = graph.sampler({ magFilter: '~a', minFilter: '~a', mipmapFilter: '~a', addressModeU: '~a', addressModeV: '~a', addressModeW: '~a', lodMaxClamp: ~a~a~a });"
              n filter filter mipmap address address address (number-text (or (opt 'max-lod) 0) (scalar-t 'float))
              (if anisotropic? ", maxAnisotropy: 16" "")
              (if (opt 'compare) (format ", compare: '~a'" (compare-function (opt 'compare))) ""))
       (line! "    const filtering_~a = ~a;" n (if (or (equal? filter "linear") (equal? mipmap "linear")) "true" "false"))]
      [(g-uniform n t source _)
       (line! "    const uniform_~a = graph.uniform(~a, ~a, inputs.~a);" n (ts-string n) (ts-layout-type t) source)]
      [(g-image n f extent _ when _)
       (define create
         (format "graph.createImage(~a, '~a', ~a)" (ts-string n) (webgpu-format f)
                 (match extent
                   [(list 'fixed w h) (format "{ fixed: [~a, ~a] }" (uint-expr w) (uint-expr h))]
                   [(list 'surface scale) (format "{ surface: ~a }" (number-text scale (scalar-t 'float)))]
                   [(list 'extent-of import) (format "{ fixed: image_~a.size }" import)])))
       (if when
           (line! "    const image_~a = ~a ? ~a : undefined;" n (bool-expr when) create)
           (line! "    const image_~a = ~a;" n create))
       (unless (internal? n) (line! "    result.images.~a = image_~a;" n n))]
      [(g-buffer n t count _ when _)
       (define create
         (format "graph.createBuffer(~a, ~a, ~a)" (ts-string n) (ts-layout-type t)
                 (if (per-frame-expression? count env)
                     (format "(frame) => ~a" (uint-expr count))
                     (uint-expr count))))
       (if when
           (line! "    const buffer_~a = ~a ? ~a : undefined;" n (bool-expr when) create)
           (line! "    const buffer_~a = ~a;" n create))
       (unless (internal? n) (line! "    result.buffers.~a = buffer_~a;" n n))]
      [(g-target n spec when _)
       (define (spec-text spec)
         (match spec
           [(t-attachments colors depth _)
            (format "{ colors: [~a], depth: ~a }"
                    (string-join
                     (for/list ([c (in-list colors)])
                       (format "{ image: ~a, load: '~a', store: '~a', clear: ~a }" (resource-ref (t-attachment-image c))
                               (t-attachment-load c) (t-attachment-store c) (color-text (t-attachment-clear c))))
                     ", ")
                    (if depth
                        (format "{ image: ~a, load: '~a', store: '~a', clear: ~a }" (resource-ref (t-attachment-image depth))
                                (t-attachment-load depth) (t-attachment-store depth) (number-text (t-attachment-clear depth) (scalar-t 'float)))
                        "null"))]
           [(t-surface clear depth _)
            (format "graph.surfaceTarget(~a, ~a)" (color-text clear) (if (eq? depth 'none) "null" (format "'~a'" (webgpu-format depth))))]
           [(t-target-if c then else _)
            (format "(~a ? ~a : ~a)" (bool-expr c) (spec-text then) (spec-text else))]))
       (if when
           (line! "    const target_~a: zgal.Target | undefined = ~a ? ~a : undefined;" n (bool-expr when) (spec-text spec))
           (line! "    const target_~a: zgal.Target = ~a;" n (spec-text spec)))]
      [(g-material n m bindings push options invocations when _)
       (define bound
         (for/list ([pair (in-list bindings)])
           (match-define (cons b resource) pair)
           (format "~a: ~a" (binding-name b)
                   (match (entry-kind (hash-ref env resource))
                     ['sampler (format "{ sampler: sampler_~a, filtering: filtering_~a }" resource resource)]
                     ['uniform (format "{ uniform: uniform_~a }" resource)]
                     [(or 'image 'import) (format "{ image: ~a }" (resource-ref resource))]
                     [_ (format "{ buffer: ~a }" (resource-ref resource))]))))
       (define given (filter cdr push))
       (define push-text
         (if (null? (material-push m))
             "null"
             (format "(frame, push) => { ~a }"
                     (string-join (for/list ([pair (in-list given)])
                                    (format "push.~a = ~a;" (field-name (car pair)) (expr (cdr pair) (field-type (car pair)))))
                                  " "))))
       (define invocations-text
         (if (and (eq? (material-kind m) 'compute) invocations)
             (format "(frame) => [~a]" (string-join (map uint-expr invocations) ", "))
             "null"))
       (define (option k) (cond [(assq k options) => cdr] [else #f]))
       (define option-fields
         (filter values
                 (list (and (option 'topology) (format "topology: '~a'" (string-replace (symbol->string (option 'topology)) "triangle-fan" "triangle-list")))
                       (and (option 'cull) (format "cull: '~a'" (case (option 'cull) [(front-and-back) "none"] [else (option 'cull)])))
                       (and (option 'front-face) (format "frontFace: '~a'" (if (eq? (option 'front-face) 'clockwise) "cw" "ccw")))
                       (and (option 'blend) (format "blend: '~a'" (option 'blend)))
                       (and (option 'depth-test) (format "depthTest: ~a" (if (eq? (option 'depth-test) 'yes) "true" "false")))
                       (and (option 'depth-write) (format "depthWrite: ~a" (if (eq? (option 'depth-write) 'yes) "true" "false")))
                       (and (option 'depth-compare) (format "depthCompare: '~a'" (compare-function (option 'depth-compare)))))))
       (define create
         (format "new zgal.MaterialInstance(graph, ~a, ~a, ~a, ~a, ~a)" (material-name m) (ts-object bound ", ")
                 push-text invocations-text (ts-object option-fields ", ")))
       (if when
           (line! "    const material_~a = ~a ? ~a : undefined;" n (bool-expr when) create)
           (line! "    const material_~a = ~a;" n create))]
      [_ (void)]))

  ;; Nodes, in their loops: a looped or extern node is a list of ids
  (for ([n (in-list nodes)] #:when (or (looped? n) (eq? (g-node-kind n) 'extern)))
    (line! "    result.nodes.~a = [];" (g-node-name n)))
  (define open-loops '())
  (define (indent) (make-string (+ 4 (* 2 (for/sum ([l (in-list open-loops)]) (length (g-loop-clauses l))))) #\space))
  (define (enter-loops! loops)
    (define shared (let count ([a open-loops] [b loops] [k 0])
                     (if (and (pair? a) (pair? b) (eq? (car a) (car b))) (count (cdr a) (cdr b) (add1 k)) k)))
    (for ([l (in-list (reverse (drop open-loops shared)))])
      (set! open-loops (drop-right open-loops 1))
      (for ([k (in-range (sub1 (length (g-loop-clauses l))) -1 -1)])
        (line! "~a~a}" (indent) (make-string (* 2 k) #\space))))
    (set! env (loop-environment base-env open-loops))
    (for ([l (in-list (drop loops shared))])
      (for ([c (in-list (g-loop-clauses l))] [k (in-naturals)])
        (define pad (string-append (indent) (make-string (* 2 k) #\space)))
        (match c
          [(loop-clause v t (list from to step) _ _)
           (line! "~afor (let loop_~a = ~a; loop_~a < ~a; loop_~a += ~a) {" pad v (expr from t) v (expr to t) v (expr step t))]
          [(loop-clause v _ #f data _)
           (line! "~afor (const loop_~a of inputs.~a) {" pad v data)])
        (set! env (loop-environment env (list (g-loop (list c) #f)))))
      (set! open-loops (append open-loops (list l)))))
  (for ([n (in-list nodes)])
    (enter-loops! (g-node-loops n))
    (define pad (indent))
    (define node-name (g-node-name n))
    (define label
      (if (looped? n)
          (format "`~a ${result.nodes.~a.length}`" node-name node-name)
          (ts-string node-name)))
    (define overrides (g-node-overrides n))
    (define (override-text)
      (if (and overrides (pair? (node-overrides-push overrides)))
          (format "(frame, push) => { ~a }"
                  (string-join (for/list ([pair (in-list (node-overrides-push overrides))])
                                 (format "push.~a = ~a;" (field-name (car pair)) (expr (cdr pair) (field-type (car pair)))))
                               " "))
          "null"))
    (define create
      (match (g-node-kind n)
        ['dispatch
         (format "new zgal.DispatchNode(~a, ~a, ~a, ~a)" label (instance-ref "material" (g-node-material n)) (override-text)
                 (if (and overrides (node-overrides-invocations overrides))
                     (format "(frame) => [~a]" (string-join (map uint-expr (node-overrides-invocations overrides)) ", "))
                     "null"))]
        ['draw
         (format "new zgal.DrawNode(~a, ~a, ~a, (frame) => ~a, ~a)" label (instance-ref "target" (g-node-target n))
                 (instance-ref "material" (g-node-material n)) (uint-expr (g-node-count n)) (override-text))]
        ['readback
         (format "new zgal.~a(~a, graph, ~a)" (if (image-kind? (g-node-subject n)) "ReadbackNode" "BufferReadbackNode")
                 label (resource-ref (g-node-subject n)))]
        ['clear
         (format "new zgal.ClearNode(~a, graph, ~a, (frame) => ~a)" label (resource-ref (first (g-node-writes n))) (uint-expr (g-node-count n)))]
        ['cpu (format "new zgal.CpuNode(~a, inputs.~a)" label (g-node-subject n))]
        ['copy
         #:when (image-kind? (first (g-node-writes n)))
         (define offset (cdr (g-node-offsets n)))
         (define extent (g-node-count n))
         (format "new zgal.CopyToImageNode(~a, graph, ~a, ~a, (frame) => ~a, (frame) => [~a, ~a], ~a)" label
                 (resource-ref (first (g-node-reads n))) (resource-ref (first (g-node-writes n)))
                 (uint-expr (car (g-node-offsets n))) (uint-expr (first offset)) (uint-expr (second offset))
                 (if extent (format "(frame) => [~a, ~a]" (uint-expr (first extent)) (uint-expr (second extent))) "null"))]
        ['copy
         (format "new zgal.CopyNode(~a, graph, ~a, ~a, (frame) => ~a, (frame) => ~a, ~a)" label
                 (resource-ref (first (g-node-reads n))) (resource-ref (first (g-node-writes n)))
                 (uint-expr (car (g-node-offsets n))) (uint-expr (cdr (g-node-offsets n)))
                 (if (g-node-count n) (format "(frame) => ~a" (uint-expr (g-node-count n))) "null"))]
        ['extern #f]))
    (line! "~a~a{" pad (if (g-node-when n) (format "if (~a) " (bool-expr (g-node-when n))) ""))
    (cond
      [(eq? (g-node-kind n) 'extern)
       (define types (entry-type (hash-ref env (g-node-subject n))))
       (define call (format "inputs.~a(~a)" (g-node-subject n)
                            (string-join (list* "graph" "result" (for/list ([a (in-list (g-node-extra n))] [t (in-list types)]) (expr a t))) ", ")))
       (if (looped? n)
           (line! "~a  result.nodes.~a.push(...~a);" pad node-name call)
           (line! "~a  result.nodes.~a = ~a;" pad node-name call))]
      [else
       (line! "~a  const node = graph.add(~a);" pad create)
       (when (g-node-enabled n)
         (line! "~a  graph.setEnabled(node, ~a);" pad (bool-expr (g-node-enabled n))))
       (when (g-node-once? n)
         (line! "~a  graph.runOnce(node);" pad))
       (if (looped? n)
           (line! "~a  result.nodes.~a.push(node);" pad node-name)
           (line! "~a  result.nodes.~a = node;" pad node-name))])
    (line! "~a}" pad))
  (enter-loops! '())
  (line! "    return result;")
  (line! "  }")
  (line! "}")
  (reverse out))

(define (color-text c)
  (format "[~a]" (string-join (map (λ (x) (number-text x (scalar-t 'float))) c) ", ")))

(define (compare-function sym)
  (case sym
    [(never) "never"] [(less) "less"] [(equal) "equal"] [(less-or-equal) "less-equal"] [(greater) "greater"]
    [(not-equal) "not-equal"] [(greater-or-equal) "greater-equal"] [(always) "always"]))
