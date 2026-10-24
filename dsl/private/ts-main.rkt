#lang racket/base
;; The TypeScript backend for a program's main: <file>.main.ts, the program's command line and app
;; (web/zgal/main.ts), run headless under Node or in a page, and for a program with a window,
;; <file>.html, the page running it. What the program's own TypeScript gives when main can't
;; (MainInputs) goes in the program's module, and that TypeScript, <name>.inputs.ts, defines
;; mainInputs.

(require racket/list
         racket/match
         racket/string
         "ast.rkt"
         "main.rkt"
         "ts-app.rkt"
         "ts-common.rkt")

(provide emit-ts-main
         emit-ts-main-inputs
         emit-ts-page
         ts-inputs-file-name
         ts-main-file-name
         ts-page-file-name)

(define (file-name-of source)
  (let-values ([(_dir name _d) (split-path source)]) (path->string name)))

;; scene.zgal: scene.zgal.main.ts, and its page, scene.zgal.html
(define (ts-main-file-name source) (string-append (file-name-of source) ".main.ts"))
(define (ts-page-file-name source) (string-append (file-name-of source) ".html"))
;; scene.zgal's own TypeScript, defining mainInputs: scene.inputs.ts, beside it
(define (ts-inputs-file-name source) (regexp-replace #rx"([.]zgal)?$" (file-name-of source) ".inputs.ts"))

;; The app's parameters no option sets, which the program's TypeScript gives
(define (given-parameters m)
  (for/list ([p (in-list (app-parameters (main-decl-app m)))]
             #:unless (for/or ([o (in-list (main-decl-options m))])
                        (or (eq? (main-option-parameter o) p) (equal? (main-option-given o) (field-name p)))))
    p))

;; What else the program's TypeScript gives: the app's extern functions and its graphs' inputs it
;; doesn't compute
(define (given-members a)
  (append (map function-decl-name (app-externs a))
          (for*/list ([r (in-list (app-runs a))]
                      [item (in-list (graph-items (graph-run-graph r)))]
                      #:when (and (graph-input? item) (not (assoc (graph-item-name item) (graph-run-bindings r)))))
            (graph-item-name item))))

;; Whether the program's TypeScript gives anything
(define (ts-inputs? m)
  (or (pair? (given-parameters m)) (pair? (given-members (main-decl-app m)))))

;; In the program's module: what its TypeScript gives the app (lines, none if nothing)
(define (emit-ts-main-inputs prog m)
  (define a (main-decl-app m))
  (if (ts-inputs? m)
      (append
       (list ""
             (format "// What the program's TypeScript gives app ~a, which its main (~a) can't:"
                     (app-name a) (ts-main-file-name (program-source prog)))
             "// the parameters no option sets, the extern functions and the graph inputs the app doesn't"
             (format "// compute. ~a defines mainInputs(context: zgal.Context), returning them or a"
                     (ts-inputs-file-name (program-source prog)))
             "// promise of them, which main calls once the context exists."
             "export interface MainInputs {")
       (for/list ([p (in-list (given-parameters m))])
         (format "  ~a: ~a;" (field-name p) (ts-value-type (field-type p))))
       (for/list ([n (in-list (given-members a))])
         (format "  ~a: ~aInputs['~a'];" n (app-name a) n))
       (list "}"))
      '()))

;; An object literal of members, "{}" without any
(define (ts-object members)
  (if (null? members) "{}" (format "{ ~a }" (string-join members ", "))))

(define (lower-first s) (string-append (string-downcase (substring s 0 1)) (substring s 1)))

;; A TypeScript literal of an option's value
(define (value-ts v type)
  (case type
    [(float) (number->string (real->double-flonum (floating-point-bytes->real (real->floating-point-bytes (exact->inexact v) 4))))]
    [(path) (format "zgal.commandLinePath(~a)" (ts-string v))]
    [else (format "~a" v)]))

(define (metavar o)
  (case (main-option-kind o)
    [(positional) (format "<~a>" (field-name (main-option-parameter o)))]
    [(flag) ""]
    [else (case (option-value-type (main-option-parameter o))
            [(int uint) "<integer>"] [(float) "<number>"] [(path) "<path>"])]))

(define (help-text o)
  (define notes
    (filter values
            (list (and (main-option-range o) (format "~a to ~a" (car (main-option-range o)) (cdr (main-option-range o))))
                  (and (main-option-default o) (format "default ~a" (main-option-default o))))))
  (if (null? notes) (main-option-help o) (format "~a (~a)" (main-option-help o) (string-join notes ", "))))

;; A parameter's value before the command line: its option's default, else false, 0 or ''
(define (initial-value p options)
  (define o (findf (λ (o) (eq? (main-option-parameter o) p)) options))
  (cond
    [(and o (main-option-default o)) (value-ts (main-option-default o) (option-value-type p))]
    [else (match (field-type p)
            [(scalar-t 'bool) "false"]
            [(opaque-t 'path) "''"]
            [_ "0"])]))

(define (emit-ts-main prog m)
  (define a (main-decl-app m))
  (define name (app-name a))
  (define window (main-decl-window m))
  (define capture (main-decl-capture m))
  (define runs (app-runs a))
  (define options (main-decl-options m))
  (define module (string-append "./" (file-name-of (program-source prog)) ".ts"))
  (define inputs? (ts-inputs? m))
  (define out '())
  (define (line! fmt . args) (set! out (cons (apply format fmt args) out)))
  (line! "// Generated by zgal from ~a, for the web. Don't edit it: edit that file." (program-source prog))
  (line! "// The program running app ~a: headless under Node (node ~a --frames N)~a." name
         (ts-main-file-name (program-source prog))
         (if window (format ", or in its page (~a, built to JavaScript)" (ts-page-file-name (program-source prog))) ""))
  (line! "import * as zgal from './zgal/index.ts';")
  (line! "import { ~a } from '~a';" name module)
  (when inputs?
    (line! "import { mainInputs } from './~a';" (ts-inputs-file-name (program-source prog))))
  (line! "")
  (line! "// What the command line sets: the app's parameters")
  (line! "export interface ~aParameters {" name)
  (for ([p (in-list (app-parameters a))])
    (line! "  ~a: ~a;" (field-name p) (ts-value-type (field-type p))))
  (line! "}")
  (line! "")
  (line! "export const program: zgal.Program<~a, ~aParameters> = {" name name)
  (line! "  name: ~a," (ts-string (file-name-of (regexp-replace #rx"[.]zgal$" (program-source prog) ""))))
  (line! "  about: ~a," (ts-string (or (main-decl-about m) "")))
  (line! "  window: ~a," (match window
                          [#f "null"]
                          [(list title w h) (format "{ title: ~a, width: ~a, height: ~a }" (ts-string title) w h)]))
  (line! "  captureOption: ~a," (if (and capture (not (third capture))) "true" "false"))
  (line! "  captureTo: ~a," (if (and capture (third capture)) (format "(inputs) => inputs.~a" (third capture)) "null"))
  (line! "  controls: [~a]," (string-join (map ts-string (main-decl-controls m)) ", "))
  (line! "  options: [~a" (if (null? options) "]," ""))
  (for ([o (in-list options)])
    (define p (main-option-parameter o))
    (define n (field-name p))
    (define type (option-value-type p))
    (define given-text (if (main-option-given o) (format "inputs.~a = true; " (main-option-given o)) ""))
    (define set
      (match (main-option-kind o)
        ['flag (format "(inputs) => { inputs.~a = true; return null; }" n)]
        [_ (match type
             ['path (format "(inputs, text) => { ~ainputs.~a = zgal.commandLinePath(text); return null; }" given-text n)]
             [_ (define range (main-option-range o))
                (format "(inputs, text) => { ~areturn zgal.parseNumber('~a', text, (v) => { inputs.~a = v; }~a); }" given-text type n
                        (if range (format ", ~a, ~a" (value-ts (car range) type) (value-ts (cdr range) type)) ""))])]))
    (line! "    { kind: '~a', name: ~a, metavar: ~a, help: ~a, set: ~a }," (main-option-kind o)
           (ts-string (or (main-option-flag o) (metavar o))) (ts-string (metavar o)) (ts-string (help-text o)) set))
  (unless (null? options) (line! "  ],"))
  (line! "  defaults: () => (~a),"
         (ts-object (for/list ([p (in-list (app-parameters a))]) (format "~a: ~a" (field-name p) (initial-value p options)))))
  (define extra
    (append (match (main-decl-timestep m)
              [#f '()]
              [(cons max-step fixed-step) (list (format "timestep: launch.timestep(~a, ~a)" (value-ts max-step 'float) (value-ts fixed-step 'float)))])
            (if (app-run-mode? a) (list "mode: launch.mode()") '())))
  (define graphs
    (if (<= (length runs) 1)
        "newGraph()"
        (format "{ ~a }" (string-join (for/list ([r (in-list runs)]) (format "~a: newGraph()" (lower-first (graph-name (graph-run-graph r))))) ", "))))
  (define launch-name (if (null? extra) "_launch" "launch"))
  (define extras (string-append* (for/list ([e (in-list extra)]) (string-append ", " e))))
  (if inputs?
      (line! "  create: async (context, ~a, parameters, newGraph) => ~a.create(context, ~a, { ...parameters, ...(await mainInputs(context))~a }),"
             launch-name name graphs extras)
      (line! "  create: (context, ~a, parameters, newGraph) => ~a.create(context, ~a, { ...parameters~a }),"
             launch-name name graphs extras))
  (match capture
    [#f (line! "  capture: null,")]
    [(list g-name node-name _)
     (define g (graph-run-graph (findf (λ (r) (equal? (graph-name (graph-run-graph r)) g-name)) runs)))
     (define node (findf (λ (n) (equal? (g-node-name n) node-name)) (graph-nodes g)))
     (define ids (if (<= (length runs) 1) "app.ids" (format "app.~aIds" (lower-first g-name))))
     (define id (if (g-node-when node)
                    (format "zgal.requireNode(~a.nodes.~a, ~a, ~a)" ids node-name (ts-string g-name) (ts-string node-name))
                    (format "~a.nodes.~a" ids node-name)))
     (line! "  // The last frame of ~a's readback ~a" g-name node-name)
     (line! "  capture: (app) => ({ graph: app.renderGraph(), node: ~a })," id)])
  (line! "};")
  (line! "")
  (when window
    (line! "// Runs the program in a canvas of a page (zgal.mountProgram), its assets in assets/ beside this module")
    (line! "export function mount(canvas: HTMLCanvasElement, options: zgal.MountOptions): zgal.MountedProgram {")
    (line! "  return zgal.mountProgram(canvas, program, new URL('assets/', import.meta.url), options);")
    (line! "}")
    (line! ""))
  (line! (if window
             (format "// Runs the program where it is: under Node with its command line, or as its page (~a)" (ts-page-file-name (program-source prog)))
             "// Runs the program under Node with its command line"))
  (line! "export function run(): Promise<void> {")
  (line! "  return zgal.runProgram(program, import.meta.url);")
  (line! "}")
  (line! "")
  (line! "// Run by Node (not imported), it runs; pages call run() or mount()")
  (line! "if (import.meta.main) await run();")
  (string-append (string-join (reverse out) "\n") "\n"))

;; The page running a program with a window: its canvas, and the main built to JavaScript
(define (emit-ts-page prog m)
  (match-define (list title _ _) (main-decl-window m))
  (define main-js (regexp-replace #rx"[.]ts$" (ts-main-file-name (program-source prog)) ".js"))
  (define (html s) (regexp-replace* #rx"[&<>\"]" s (λ (c) (case c [("&") "&amp;"] [("<") "&lt;"] [(">") "&gt;"] [else "&quot;"]))))
  (string-append
   "<!doctype html>\n"
   (format "<!-- Generated by zgal from ~a. Don't edit it: edit that file. Options go in the query string,\n" (html (program-source prog)))
   "     e.g. ?frames=60 (arg=... for positional arguments). -->\n"
   "<html lang=\"en\">\n"
   "<head>\n"
   "<meta charset=\"utf-8\">\n"
   "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
   (format "<title>~a</title>\n" (html title))
   "<style>\n"
   "  html, body { margin: 0; height: 100%; background: #000; color: #ddd; font: 14px system-ui, sans-serif; }\n"
   "  body { overflow: hidden; }\n"
   "  #zgal-message { position: fixed; left: 1em; bottom: 1em; margin: 0; white-space: pre-wrap; }\n"
   "</style>\n"
   "</head>\n"
   "<body>\n"
   "<canvas id=\"zgal\"></canvas>\n"
   (format "<script type=\"module\">import { run } from './~a'; run();</script>\n" (html main-js))
   "</body>\n"
   "</html>\n"))
