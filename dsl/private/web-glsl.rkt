#lang racket/base
;; GLSL shaders for the web: a material whose shaders are written in GLSL (bodies in `shaders`), or
;; whose ZGAL bodies call GLSL functions (extern-glsl), becomes WGSL through glslc (to SPIR-V) and
;; naga (to WGSL). Each shader is the GLSL backend's (a GLSL body, or a ZGAL body with the functions
;; it uses and the files of the GLSL ones, as natively), with its web interface (glsl.rkt's
;; web-interface): one descriptor per binding, set 0 and binding i for the material's i'th binding,
;; and the push constants a uniform block at the binding after them, as the WGSL backend lays out a
;; material's bind group (wgsl.rkt), so the TypeScript runtime binds the module as it binds a ZGAL
;; body's. A material goes through GLSL as a whole, as its shaders share one bind group.
;;
;; Errors keep pointing at the source: glslc reports its errors through the shader's #line
;; directives, at the body's lines (or the .zgal file's, for the interface and ZGAL bodies, or the
;; GLSL function's file), which become the error's location; naga's SPIR-V reader reports no lines,
;; so what it can't translate is reported at the shader's declaration. What WGSL lacks and the WGSL
;; backend emulates for ZGAL bodies (8- and 16-bit integers, float atomics, subgroup operations) and
;; arrays of bindings, which naga and WebGPU lack, are refused before the tools run, where the
;; program uses them.
;;
;; --web-out translates each such material once (translate-web-material), and its TypeScript
;; describes the material from the result (web-material-bindings): one binding per ZGAL binding, as
;; the GLSL interface declares them, without the sampler variants and overridable constants a ZGAL
;; body's module has (wgsl.rkt). Its samplers are WebGPU's own, so a comparison compares in the
;; sampler and a sampler clamping to a border has no border colour (the runtime refuses one).

(require racket/file
         racket/list
         racket/match
         racket/port
         racket/set
         racket/string
         racket/system
         "ast.rkt"
         "body.rkt"
         "builtins.rkt"
         "errors.rkt"
         "glsl.rkt"
         "layout.rkt"
         (only-in "wgsl.rkt" wgsl-binding wgsl-binding-source body-opaque-uses opaque-name))

(provide (struct-out web-toolchain)
         (struct-out web-wgsl)
         (struct-out web-material)
         emit-web-shader
         web-shader->wgsl
         translate-web-material
         web-material-module
         glsl-material?
         spirv-used-bindings
         spirv-mark-readonly-buffers)

;; The tools: glslc and naga (paths, or names found on PATH), and the directories glslc finds
;; shader bodies' #includes in
(struct web-toolchain (glslc naga include-dirs) #:transparent)

;; A translated shader: its WGSL module; the bindings (@binding indices) its code uses, which the
;; module declares along with the unused ones (glslc keeps every declaration, so the runtime's bind
;; group layout takes its stages from here); and what the tools warned about (their output when they
;; succeeded, "" for nothing)
(struct web-wgsl (text used warnings) #:transparent)

(define (web-error where fmt . args)
  (apply zgal-error 'not-on-web where fmt args))

;; The GLSL functions (extern-glsl) ZGAL shader s calls, directly or through ZGAL functions
(define (glsl-functions-called prog s)
  (define declarations
    (for/hash ([d (in-list (program-functions prog))] #:when (function-decl? d)) (values (function-decl-name d) d)))
  (for*/list ([name (in-list (zgal-shader-uses s))]
              [d (in-value (hash-ref declarations name #f))]
              #:when (glsl-function-decl? d))
    d))

;; Whether material m is compiled through GLSL on the web: it has GLSL bodies, or one of its ZGAL
;; bodies calls a GLSL function
(define (glsl-material? prog m)
  (for/or ([s (in-list (material-shaders m))])
    (or (and (shader-path s) #t)
        (pair? (glsl-functions-called prog s)))))

;; For messages about material m, compiled through GLSL: (values what is compiled from GLSL, the
;; remedy)
(define (glsl-subject prog m)
  (define called
    (remove-duplicates
     (for*/list ([s (in-list (material-shaders m))] #:unless (shader-path s)
                 [d (in-list (glsl-functions-called prog s))])
       (function-decl-name d))))
  (define callers
    (for/list ([s (in-list (material-shaders m))]
               #:when (and (not (shader-path s)) (pair? (glsl-functions-called prog s))))
      (shader-name s)))
  (define glsl-bodies? (ormap shader-path (material-shaders m)))
  (cond
    [(andmap shader-path (material-shaders m)) (values "a GLSL shader" "write the shader in ZGAL")]
    [else
     (define reasons
       (append (if glsl-bodies? '("it has GLSL bodies") '())
               (if (pair? called) (list (format "~a ~a ~a, written in GLSL" (string-join callers " and ") (if (= (length callers) 1) "calls" "call") (string-join called ", "))) '())))
     (values (format "~a's shaders (compiled from GLSL on the web, as ~a)" (material-name m) (string-join reasons " and "))
             (format "write ~a in ZGAL" (string-join (append (if glsl-bodies? '("its GLSL bodies") '()) called) ", ")))]))

;; The web-flavoured GLSL of shader s of material m: emit-shader's text with the web interface.
;; body (a GLSL body's text, or #f for a ZGAL body), out-path, glsl-files: as emit-shader's.
;; storage-format: a storage image binding's GLSL format qualifier, e.g. "rgba8" (the WGSL
;; declares the format: see wgsl.rkt's emit-wgsl). Refuses what the interface can't express on the
;; web, and what a ZGAL body (and the functions it calls) uses that WGSL lacks and only the WGSL
;; backend emulates.
(define (emit-web-shader prog m s body out-path #:storage-format storage-format #:glsl-files [glsl-files (hash)])
  (define-values (subject remedy) (glsl-subject prog m))
  ;; The native interface's descriptor indexing, which the web interface replaces
  (when body
    (for ([line (in-list (string-split body "\n" #:trim? #f))] [number (in-naturals 1)])
      (when (regexp-match? #px"^\\s*#\\s*include\\s*[\"<]bindless[.]glsl[\">]" line)
        (web-error (loc (shader-path s) number 1) "~a includes bindless.glsl, the native interface's descriptor indexing, which WebGPU lacks: reach bindings through their accessors (name()), which the web interface defines"
                   (shader-name s)))))
  (for ([b (in-list (material-bindings m))])
    (when (eq? (binding-kind b) 'array)
      (web-error (binding-loc b) "~a is an array of bindings, which ~a can't index on the web (WebGPU has no descriptor indexing): ~a"
                 (binding-name b) subject remedy))
    (define t (binding-buffer-type b))
    (when (and t (pair? (small-integer-kinds-in t)))
      (web-error (binding-loc b) "~a holds ~a, and WGSL has no 8- and 16-bit integers: ~a can't read them on the web (the WGSL backend reads them from 32-bit words), so use uint or int, or ~a"
                 (binding-name b) (type-name t) subject remedy)))
  (for ([f (in-list (material-shared m))])
    (when (pair? (small-integer-kinds-in (field-type f)))
      (web-error (field-loc f) "shared variable ~a holds ~a, and WGSL has no 8- and 16-bit integers: ~a can't use them on the web, so use uint or int, or ~a"
                 (field-name f) (type-name (field-type f)) subject remedy)))
  ;; ZGAL bodies read structs and arrays from a uniform-layout mirror (wgsl.rkt's push-mirror?),
  ;; which the GLSL interface's std430 block doesn't follow
  (for ([f (in-list (material-push m))])
    (define t (field-type f))
    (when (or (array-t? t) (struct-t? t))
      (web-error (field-loc f) "push constant ~a is ~a, but the push constants of ~a on the web (a uniform block) are scalars, vectors and matrices: ~a"
                 (field-name f) (type-name t) subject remedy)))
  (unless body (check-zgal-body prog s subject remedy))
  (emit-shader prog m s body out-path #:interface (web-interface storage-format) #:glsl-files glsl-files))

;; Refuses what ZGAL body s, and the functions it calls, use that WGSL lacks, which the WGSL backend
;; emulates (wgsl.rkt) but a shader compiled from GLSL can't: 8- and 16-bit integers (also in the
;; functions' signatures, GLSL ones' included), float atomics (WGSL's atomics are integers'),
;; subgroup operations (which also need the variant without subgroups), and a sampler used both to
;; compare with depths and to sample, or a texture both compared and sampled or fetched (WebGPU
;; binds each as one or the other: check-comparisons). Integer atomics are naga's: it declares what
;; they act on atomic.
(define (check-zgal-body prog s subject remedy)
  (define declarations
    (for/hash ([d (in-list (program-functions prog))])
      (values (if (function-decl? d) (function-decl-name d) (constant-decl-name d)) d)))
  (define used (for/list ([name (in-list (zgal-shader-uses s))]) (hash-ref declarations name)))
  (for ([d (in-list used)])
    (define-values (name types where)
      (match d
        [(function-decl name params result _ _ _ l) (values name (cons result (map cdr params)) l)]
        [(constant-decl name type _ _ _ l) (values name (list type) l)]))
    (define t (for/first ([t (in-list types)] #:when (pair? (small-integer-kinds-in t))) t))
    (when t
      (web-error where "~a's type has ~a, and WGSL has no 8- and 16-bit integers: ~a can't use them on the web (the WGSL backend computes with them in 32 bits), so use uint or int, or ~a"
                 name (type-name t) subject remedy)))
  (define bodies
    (cons (zgal-shader-body s)
          (filter values (for/list ([d (in-list used)]) (if (function-decl? d) (function-decl-body d) (constant-decl-body d))))))
  (for* ([body (in-list bodies)] [x (in-list (subexpressions body))])
    (define t (texpr-type x))
    (when (pair? (small-integer-kinds-in t))
      (web-error (texpr-loc x) "this is ~a, and WGSL has no 8- and 16-bit integers: ~a can't compute with them on the web (the WGSL backend computes with them in 32 bits), so use uint or int, or ~a"
                 (type-name t) subject remedy))
    (match x
      [(t-atomic op _ _ (scalar-t 'float) l)
       (web-error l "this atomic ~a is on a float, and WGSL's atomics are integers': ~a can't do it on the web (the WGSL backend emulates it with a compare-and-exchange loop), so ~a"
                  op subject remedy)]
      [(or (t-call (? subgroup-category) _ _ l) (t-global 'stage (pregexp #px"^subgroup") _ _ l))
       (web-error l "this is a subgroup operation, which ~a can't use on the web (the WGSL backend emulates what WGSL lacks of them, with a variant for devices without subgroups): ~a"
                  subject remedy)]
      [_ (void)]))
  (check-comparisons s subject remedy (body-opaque-uses prog (zgal-shader-body s))))

;; Refuses a sampler ZGAL body s both compares with and samples with, and a texture it both compares
;; with depths and samples or fetches, through the functions it calls too: the WGSL backend binds a
;; sampler as a variant for each use (and reads a compared texture, a depth texture, as one), but a
;; shader compiled from GLSL has one binding for each, which WebGPU binds as one or the other.
;; uses: the body's opaque-uses (body-opaque-uses). Reported at the body's last use of the binding
;; (where it uses it, or passes it to a function).
(define (check-comparisons s subject remedy uses)
  (define body (zgal-shader-body s))
  (define (last-use name)
    (define locs
      (for/list ([e (in-list (subexpressions body))]
                 #:when (match e
                          [(or (t-texture _ args _ _) (t-user-call _ args _ _))
                           (for/or ([a (in-list args)]) (equal? (opaque-name a) name))]
                          [_ #f]))
        (texpr-loc e)))
    (when (null? locs)
      (error 'web-glsl "internal error: ~a's body doesn't use ~a" (shader-name s) name))
    (argmax (λ (l) (+ (* (loc-line l) 100000) (loc-column l))) locs))
  (for ([name (in-list (sort (hash-keys uses) string<?))])
    (define variants (hash-ref uses name))
    (when (set-member? variants 'compare)
      ;; A sampler sampling a texture a function compares has the depth variant
      (when (or (set-member? variants 'sample) (set-member? variants 'depth))
        (web-error (last-use name) "sampler ~a both compares with depths and samples in ~a, and WebGPU binds a sampler as one or the other: ~a can't use it both ways on the web (the WGSL backend binds a variant of it for each), so bind two samplers, or ~a"
                   name (shader-name s) subject remedy))
      (when (set-member? variants 'read)
        (web-error (last-use name) "texture ~a is both compared with depths and sampled or fetched in ~a, and WebGPU binds a texture as one or the other: ~a can't use it both ways on the web (the WGSL backend reads it as a depth texture), so bind it twice, or ~a"
                   name (shader-name s) subject remedy)))))

;; Runs a program, returning its exit code and what it wrote to stdout and stderr (one port, so
;; a tool writing more errors than a pipe holds can't block while the other pipe is read). A tool
;; that isn't there is the toolchain's problem, not the program's: not an exn:zgal.
(define (run program . arguments)
  ;; A path given (e.g. zgal.mk's naga, before it's built) is checked too: subprocess would
  ;; otherwise start nothing and report it as the tool's own failure, i.e. the program's.
  (define path
    (or (if (absolute-path? program)
            (and (file-exists? program) (memq 'execute (file-or-directory-permissions program)) program)
            (find-executable-path program))
        (error 'web-glsl "~a isn't installed, or isn't on PATH" program)))
  (define-values (process out in _err) (apply subprocess #f #f 'stdout path arguments))
  (close-output-port in)
  (define output (port->string out))
  (subprocess-wait process)
  (close-input-port out)
  (values (subprocess-status process) output))

;; Translates the web-flavoured GLSL `glsl` of shader s (from emit-web-shader) to WGSL. SPIR-V 1.5:
;; glslc compiles `discard` for Vulkan 1.3 to OpDemoteToHelperInvocation, which naga doesn't read.
;;
;; glslc's errors are raised as glsl-error at the line they name (through #line: the body's, or the
;; .zgal file's for the interface), with the first error's message and how many it reported; its
;; other failures (e.g. an include directory missing) at the shader's declaration. What naga can't
;; translate is not-on-web at the shader's declaration. glsl-error isn't in the catalog: it's
;; raised directly here, as its example would need glslc and naga wherever the documentation (which
;; checks the catalog's examples) is built.
(define (web-shader->wgsl toolchain s glsl)
  (define directory (make-temporary-directory "zgal-web-glsl-~a"))
  (dynamic-wind
   void
   (λ ()
     (define name (shader-name s))
     (define source (build-path directory name))
     (define spv (build-path directory (string-append name ".spv")))
     (define wgsl (build-path directory (string-append name ".wgsl")))
     (call-with-output-file source (λ (out) (void (write-string glsl out))))
     (define-values (glslc-status glslc-output)
       (apply run (web-toolchain-glslc toolchain)
              (append (for/list ([d (in-list (web-toolchain-include-dirs toolchain))]) (format "-I~a" d))
                      (list "--target-env=vulkan1.3" "--target-spv=spv1.5" (path->string source) "-o" (path->string spv)))))
     (unless (zero? glslc-status)
       (raise-glslc-error s glslc-output))
     (define module (spirv-mark-readonly-buffers (file->bytes spv)))
     (call-with-output-file spv #:exists 'truncate (λ (out) (void (write-bytes module out))))
     (define-values (naga-status naga-output)
       (run (web-toolchain-naga toolchain) (path->string spv) (path->string wgsl)))
     (unless (zero? naga-status)
       (web-error (shader-loc s) "naga can't translate ~a to WGSL: ~a~a" name (naga-message naga-output)
                  (if (regexp-match? #rx"invalid type width|scalar width [12] is not supported" naga-output)
                      " (WGSL has no 8- and 16-bit integers)"
                      "")))
     (web-wgsl (file->string wgsl) (spirv-used-bindings module)
               (string-append glslc-output (naga-message naga-output))))
   (λ () (delete-directory/files directory))))

;; naga's output as one line per message: without its logger's warnings about capabilities it
;; doesn't know and its line saying validation failed (the message before it says why), and a
;; message's continuation lines (indented) joined to it
(define (naga-message output)
  (define lines
    (for/list ([l (in-list (string-split output "\n"))]
               #:unless (regexp-match? #rx"^\\[WARN |requires validation to succeed" l))
      l))
  (string-join (for/list ([l (in-list lines)]) (string-trim l)) " "))

;; glslc's "file:line: error: message" lines: the first is where the error is reported
(define (raise-glslc-error s output)
  (define errors (regexp-match* #px"(?m:^(.*?):([0-9]+): error: (.*)$)" output #:match-select cdr))
  (define (fail where fmt . args)
    (raise (exn:zgal (apply format fmt args) (current-continuation-marks) 'glsl-error where)))
  (cond
    [(null? errors) (fail (shader-loc s) "glslc failed on ~a: ~a" (shader-name s) (string-trim output))]
    [else
     (match-define (list file line message) (first errors))
     (fail (loc file (string->number line) 1) "~a~a" (string-trim message)
           (if (> (length errors) 1) (format " (glslc reports ~a errors in all)" (length errors)) ""))]))

;; A SPIR-V module's words, and whether they're big-endian
(define (spirv-words who bytes)
  (define count (quotient (bytes-length bytes) 4))
  (define (word-at i big-endian?) (integer-bytes->integer bytes #f big-endian? (* 4 i) (* 4 (add1 i))))
  (define big-endian?
    (cond [(and (>= count 5) (= (word-at 0 #f) #x07230203)) #f]
          [(and (>= count 5) (= (word-at 0 #t) #x07230203)) #t]
          [else (error who "not a SPIR-V module")]))
  (values (for/vector #:length count ([i (in-range count)]) (word-at i big-endian?)) big-endian?))

;; A SPIR-V module's instructions after its header, in order: each a vector of its opcode and then
;; its operands, with the index of its first word
(define (spirv-instructions who words)
  (let loop ([i 5] [instructions '()])
    (cond
      [(>= i (vector-length words)) (reverse instructions)]
      [else
       (define head (vector-ref words i))
       (define count (arithmetic-shift head -16))
       (when (or (zero? count) (> (+ i count) (vector-length words))) (error who "malformed SPIR-V"))
       (define instruction (for/vector #:length count ([k (in-range count)])
                             (if (zero? k) (bitwise-and head #xFFFF) (vector-ref words (+ i k)))))
       (loop (+ i count) (cons (cons i instruction) instructions))])))

;; The @binding indices of the descriptors the SPIR-V module's functions use: the variables
;; decorated with a binding that an instruction loads, stores, indexes, takes the length of, points
;; into (image atomics), copies or passes to a function. glslc keeps the declarations a shader
;; doesn't use, and so does naga.
(define (spirv-used-bindings bytes)
  (define-values (words _big-endian?) (spirv-words 'spirv-used-bindings bytes))
  (define bindings (make-hasheqv))   ; a variable's id -> its binding
  (define used (mutable-seteqv))     ; the ids used
  (for ([at+instruction (in-list (spirv-instructions 'spirv-used-bindings words))])
    (define instruction (cdr at+instruction))
    (define count (vector-length instruction))
    (define (operand k) (vector-ref instruction k))
    (define (use! k) (when (< k count) (set-add! used (operand k))))
    (case (operand 0)
      ;; OpDecorate target Binding value
      [(71) (when (and (>= count 4) (= (operand 2) 33)) (hash-set! bindings (operand 1) (operand 3)))]
      ;; OpLoad, OpAccessChain, OpInBoundsAccessChain, OpPtrAccessChain, OpInBoundsPtrAccessChain,
      ;; OpArrayLength, OpImageTexelPointer, OpCopyObject, and the atomics with a result: the
      ;; pointer is the third operand
      [(61 65 66 67 70 68 60 83
        227 229 230 231 232 233 234 235 236 237 238 239 240 241 242 318 5614 5615 6035)
       (use! 3)]
      ;; OpStore, OpAtomicStore, OpAtomicFlagClear: the first
      [(62 228 319) (use! 1)]
      ;; OpCopyMemory: target and source
      [(63) (use! 1) (use! 2)]
      ;; OpFunctionCall: the arguments
      [(57) (for ([k (in-range 4 count)]) (use! k))]
      [else (void)]))
  (for/set ([(id binding) (in-hash bindings)] #:when (set-member? used id)) binding))

;; The SPIR-V module with its readonly buffers' variables decorated NonWritable, as their blocks'
;; members are. naga takes a storage buffer's access from the variable's decoration alone, and
;; glslang only adds it beside the members' in newer releases: Ubuntu 24.04's glslc (shaderc
;; 2023.8) leaves it out, so naga declared readonly buffers read_write, which the bind group's
;; read-only storage layout then refused.
(define (spirv-mark-readonly-buffers bytes)
  (define who 'spirv-mark-readonly-buffers)
  (define-values (words big-endian?) (spirv-words who bytes))
  (define instructions (spirv-instructions who words))
  (define non-writable 24)
  (define storage-buffer 12)
  (define member-counts (make-hasheqv))       ; a struct's id -> its member count
  (define readonly-members (make-hasheqv))    ; a struct's id -> its NonWritable members
  (define readonly-variables (mutable-seteqv))
  (define pointers (make-hasheqv))            ; a pointer type's id -> its pointee, to storage buffers
  (define variables '())                      ; storage buffer variables: (id . pointer type)
  (for ([at+instruction (in-list instructions)])
    (define instruction (cdr at+instruction))
    (define (operand k) (vector-ref instruction k))
    (case (operand 0)
      [(30) (hash-set! member-counts (operand 1) (- (vector-length instruction) 2))]
      [(71) (when (= (operand 2) non-writable) (set-add! readonly-variables (operand 1)))]
      [(72) (when (= (operand 3) non-writable)
              (hash-update! readonly-members (operand 1) (λ (members) (set-add members (operand 2))) (seteqv)))]
      [(32) (when (= (operand 2) storage-buffer) (hash-set! pointers (operand 1) (operand 3)))]
      [(59) (when (= (operand 3) storage-buffer) (set! variables (cons (cons (operand 2) (operand 1)) variables)))]
      [else (void)]))
  (define unmarked
    (sort (for/list ([v (in-list variables)]
                     #:unless (set-member? readonly-variables (car v))
                     #:when (let ([block (hash-ref pointers (cdr v) #f)])
                              (and block
                                   (positive? (hash-ref member-counts block 0))
                                   (= (set-count (hash-ref readonly-members block (seteqv)))
                                      (hash-ref member-counts block)))))
            (car v))
          <))
  ;; The decorations go before the first annotation, so within the annotations' section
  (define annotations-start
    (for/first ([at+instruction (in-list instructions)] #:when (memv (vector-ref (cdr at+instruction) 0) '(71 72)))
      (car at+instruction)))
  (cond
    [(or (null? unmarked) (not annotations-start)) bytes]
    [else
     (define added (append* (for/list ([id (in-list unmarked)]) (list (bitwise-ior (arithmetic-shift 3 16) 71) id non-writable))))
     (define (encode ws) (apply bytes-append (for/list ([w ws]) (integer->integer-bytes w 4 #f big-endian?))))
     (bytes-append (subbytes bytes 0 (* 4 annotations-start))
                   (encode added)
                   (subbytes bytes (* 4 annotations-start)))]))

;; ---------------------------------------------------------------------------------------------
;; Materials

;; A material compiled through GLSL (glsl-material?), translated. bindings: its bindings as WebGPU
;; binds them (wgsl-binding), one per ZGAL binding, the binding's index its @binding: a sampler a shader
;; compares with (naga's sampler_comparison, from a sampler2DShadow) is a 'comparison-sampler, and
;; with a texture compared with depths (texture_depth_2d) has depth?; the stages are those whose
;; code uses it. modules: (list (cons stage WGSL) ...) in the material's order, the storage
;; images' formats placeholders that web-material-module fills in.
(struct web-material (bindings modules) #:transparent)

(define format-marker "\u0001")

;; Translates material m's shaders to WGSL through GLSL (web-shader->wgsl, with toolchain): its GLSL
;; bodies, and its ZGAL bodies with the functions they call. read-body: a GLSL body's text;
;; glsl-files: the texts of the program's GLSL functions' files, by path (emit-shader's). GLSL bodies'
;; #includes are found as natively (zgal.mk): in the shaders directory beside the .zgal file (as
;; zgal.mk's native rules have them), then the toolchain's.
(define (translate-web-material toolchain prog m read-body #:glsl-files glsl-files)
  (define-values (source-dir _name _dir?) (split-path (path->complete-path (program-source prog))))
  (define include-dirs
    (remove-duplicates
     (filter directory-exists?
             (cons (path->string (simplify-path (build-path source-dir "shaders")))
                   (web-toolchain-include-dirs toolchain)))))
  (define tools (struct-copy web-toolchain toolchain [include-dirs include-dirs]))
  ;; Storage images as rgba8 (an image2D's), whose WGSL format is then replaced
  (define translated
    (for/list ([s (in-list (material-shaders m))])
      (define glsl (emit-web-shader prog m s (and (shader-path s) (read-body s)) (shader-name s)
                                    #:storage-format (λ (b) "rgba8") #:glsl-files glsl-files))
      (cons (shader-stage s) (web-shader->wgsl tools s glsl))))
  (define (declaration-pattern index)
    (pregexp (format "@group\\(0\\) @binding\\(~a\\)\\s*var(?:<[^>]*>)? [A-Za-z0-9_]+: ([^;]+);" index)))
  ;; The WGSL types the stages using binding `index` declare it with
  (define (declared-types index)
    (remove-duplicates
     (for/list ([t (in-list translated)] #:when (set-member? (web-wgsl-used (cdr t)) index))
       (define declared (regexp-match (declaration-pattern index) (web-wgsl-text (cdr t))))
       (unless declared
         (error 'translate-web-material "internal error: ~a's module doesn't declare binding ~a" (material-name m) index))
       (cadr declared))))
  (define bindings
    (for/list ([b (in-list (material-bindings m))] [index (in-naturals)])
      (define types (declared-types index))
      (define (compared? wgsl-type)
        (define compared (member wgsl-type types))
        (when (and compared (> (length types) 1))
          (web-error (binding-loc b) "~a is compared with depths in one of ~a's shaders and sampled in another, and WebGPU binds it as one or the other: ~a's bodies are GLSL, which translates with one binding per declared resource, where ZGAL bodies get a binding per use. Compare in both, bind two, or write the bodies in ZGAL"
                     (binding-name b) (material-name m) (material-name m)))
        (and compared #t))
      (define-values (kind depth?)
        (case (binding-kind b)
          [(sampler) (if (compared? "sampler_comparison") (values 'comparison-sampler #t) (values 'sampler #f))]
          [(texture) (values 'texture (compared? "texture_depth_2d"))]
          [else (values (binding-kind b) #f)]))
      (wgsl-binding (binding-name b) b index kind (binding-access b)
                    (for/list ([t (in-list translated)] #:when (set-member? (web-wgsl-used (cdr t)) index)) (car t))
                    depth?)))
  (define modules
    (for/list ([t (in-list translated)])
      (cons (car t)
            (for/fold ([text (web-wgsl-text (cdr t))])
                      ([b (in-list (material-bindings m))] [index (in-naturals)] #:when (eq? (binding-kind b) 'storage-image))
              (regexp-replace (pregexp (format "(@group\\(0\\) @binding\\(~a\\)\\s*var [A-Za-z0-9_]+: texture_storage_2d<)[a-z0-9]+" index))
                              text
                              (string-append "\\1" format-marker (binding-name b) format-marker))))))
  (web-material bindings modules))

;; The WGSL of a translated material's stage: storage-format gives a storage image binding's texel
;; format (e.g. "rgba8unorm"), as emit-wgsl's does
(define (web-material-module wm stage #:storage-format storage-format)
  (define bindings (for/hash ([wb (in-list (web-material-bindings wm))]) (values (binding-name (wgsl-binding-source wb)) (wgsl-binding-source wb))))
  (regexp-replace* (pregexp (format "~a([A-Za-z0-9_]+)~a" format-marker format-marker))
                   (cdr (assq stage (web-material-modules wm)))
                   (λ (_ name) (storage-format (hash-ref bindings name)))))
