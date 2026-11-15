#lang racket/base
;; GLSL shaders for the web: a material stage written in GLSL (a GLSL body in `shaders`) becomes
;; WGSL through glslc (to SPIR-V) and naga (to WGSL). The shader is the GLSL backend's, with its web
;; interface (glsl.rkt's web-interface): one descriptor per binding, set 0 and binding i for the
;; material's i'th binding, and the push constants a uniform block at the binding after them, as
;; the WGSL backend lays out a material's bind group (wgsl.rkt), so the TypeScript runtime binds the
;; module as it binds a ZGAL body's.
;;
;; Errors keep pointing at the source: glslc reports its errors through the shader's #line
;; directives, at the body's lines (or the .zgal file's, for the interface), which become the
;; error's location; naga's SPIR-V reader reports no lines, so what it can't translate is reported at
;; the shader's declaration. 8- and 16-bit integers and arrays of bindings, which naga and WebGPU
;; lack, are refused before the tools run.
;;
;; --web-out doesn't use this yet: GLSL bodies still report not-on-web there.

(require racket/file
         racket/list
         racket/match
         racket/port
         racket/set
         racket/string
         racket/system
         "ast.rkt"
         "errors.rkt"
         "glsl.rkt"
         "layout.rkt"
         (only-in "wgsl.rkt" wgsl-push-layout))

(provide (struct-out web-toolchain)
         (struct-out web-wgsl)
         emit-web-shader
         web-shader->wgsl
         spirv-used-bindings)

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

;; The web-flavoured GLSL of shader s of material m: emit-shader's text with the web interface.
;; body, out-path, glsl-files: as emit-shader's. storage-format: a storage image binding's GLSL
;; format qualifier, e.g. "rgba8" (the WGSL declares the format: see wgsl.rkt's emit-wgsl).
;; Refuses what the interface can't express on the web.
(define (emit-web-shader prog m s body out-path #:storage-format storage-format #:glsl-files [glsl-files (hash)])
  (for ([b (in-list (material-bindings m))])
    (when (eq? (binding-kind b) 'array)
      (web-error (binding-loc b) "~a is an array of bindings, which a GLSL shader can't index on the web (WebGPU has no descriptor indexing): write the shader in ZGAL"
                 (binding-name b)))
    (define t (binding-buffer-type b))
    (when (and t (pair? (small-integer-kinds-in t)))
      (web-error (binding-loc b) "~a holds ~a, and WGSL has no 8- and 16-bit integers: a GLSL shader can't read them on the web (ZGAL bodies read them from 32-bit words), so use uint or int, or write the shader in ZGAL"
                 (binding-name b) (type-name t))))
  (for ([f (in-list (material-shared m))])
    (when (pair? (small-integer-kinds-in (field-type f)))
      (web-error (field-loc f) "shared variable ~a holds ~a, and WGSL has no 8- and 16-bit integers: a GLSL shader can't use them on the web, so use uint or int, or write the shader in ZGAL"
                 (field-name f) (type-name (field-type f)))))
  (wgsl-push-layout m)
  (emit-shader prog m s body out-path #:interface (web-interface storage-format) #:glsl-files glsl-files))

;; Runs a program, returning its exit code and what it wrote to stdout and stderr (one port, so
;; a tool writing more errors than a pipe holds can't block while the other pipe is read). A tool
;; that isn't there is the toolchain's problem, not the program's: not an exn:zgal.
(define (run program . arguments)
  (define path
    (or (if (absolute-path? program) program (find-executable-path program))
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
;; translate is not-on-web at the shader's declaration. glsl-error isn't in the catalog yet: it's
;; raised directly here, and joins the catalog (with an example needing the tools) when --web-out
;; reaches this path.
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
     (define-values (naga-status naga-output)
       (run (web-toolchain-naga toolchain) (path->string spv) (path->string wgsl)))
     (unless (zero? naga-status)
       (web-error (shader-loc s) "naga can't translate ~a to WGSL: ~a~a" name (naga-message naga-output)
                  (if (regexp-match? #rx"invalid type width|scalar width [12] is not supported" naga-output)
                      " (WGSL has no 8- and 16-bit integers)"
                      "")))
     (web-wgsl (file->string wgsl) (spirv-used-bindings (file->bytes spv))
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

;; The @binding indices of the descriptors the SPIR-V module's functions use: the variables
;; decorated with a binding that an instruction loads, stores, indexes, takes the length of, points
;; into (image atomics), copies or passes to a function. glslc keeps the declarations a shader
;; doesn't use, and so does naga.
(define (spirv-used-bindings bytes)
  (define words (quotient (bytes-length bytes) 4))
  (define (word-at i big-endian?) (integer-bytes->integer bytes #f big-endian? (* 4 i) (* 4 (add1 i))))
  (define big-endian?
    (cond [(and (>= words 5) (= (word-at 0 #f) #x07230203)) #f]
          [(and (>= words 5) (= (word-at 0 #t) #x07230203)) #t]
          [else (error 'spirv-used-bindings "not a SPIR-V module")]))
  (define (word i) (word-at i big-endian?))
  (define bindings (make-hasheqv))   ; a variable's id -> its binding
  (define used (mutable-seteqv))     ; the ids used
  (let loop ([i 5])
    (when (< i words)
      (define head (word i))
      (define count (arithmetic-shift head -16))
      (define opcode (bitwise-and head #xFFFF))
      (when (zero? count) (error 'spirv-used-bindings "malformed SPIR-V"))
      (define (operand k) (word (+ i k)))
      (define (use! k) (when (< k count) (set-add! used (operand k))))
      (case opcode
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
        [else (void)])
      (loop (+ i count))))
  (for/set ([(id binding) (in-hash bindings)] #:when (set-member? used id)) binding))
