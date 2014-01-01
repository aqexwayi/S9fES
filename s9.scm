;;
;; Scheme 9 from Empty Space
;; By Nils M Holm, 2007-2010
;; Placed in the Public Domain
;;

;; Some obvious procedures first

(define (void) (if #f #f))

(define call-with-current-continuation call/cc)

;; Auxiliary definitions, will be redefined later

(define append append2)

; There is no LET or LETREC yet, so

(define-syntax (let bindings . exprs)
  ((lambda (split)
     ((lambda (tmp-split)
        (set! split tmp-split)
        (apply (lambda (vars args)
                 (append
                   (list (append
                           (list 'lambda)
                           (append (list vars)
                                   exprs)))
                   args))
               (split bindings '() '())))
      (lambda (bind* vars args)
        (if (null? bind*)
            (list vars args)
            (split (cdr bind*)
                   (cons (caar bind*) vars)
                   (cons (cadr (car bind*)) args))))))
   #f))

(define (map-car f a)
  (let ((mapcar1 #f))
    (let ((tmp-mapcar1
            (lambda (a)
              (if (null? a)
                  '()
                   (cons (f (car a))
                         (mapcar1 (cdr a)))))))
    (set! mapcar1 tmp-mapcar1)
    (mapcar1 a))))

(define (map f a b)
  (let ((map2 #f))
    (let ((tmp-map2
            (lambda (a b)
              (if (null? a)
                  '()
                   (cons (f (car a) (car b))
                         (map2 (cdr a) (cdr b)))))))
    (set! map2 tmp-map2)
    (map2 a b))))

(define-syntax (letrec bindings . exprs)
  (let ((append3
          (lambda (a b c)
            (append a (append b c))))
        (tmps (map-car (lambda (x) (gensym)) bindings))
        (vars (map-car car bindings))
        (args (map-car cadr bindings)))
    (let ((undefineds   (map-car (lambda (v) (list v #f))
                                 vars))
          (tmp-bindings (map list tmps args))
          (updates      (map (lambda (v t) (list 'set! v t))
                             vars
                             tmps)))
      (list 'let
            undefineds
            (append3 '(let)
                     (list tmp-bindings)
                     (append updates exprs))))))

;; Type predicates

(define number? integer?)

(define (port? x)
  (or (input-port? x)
      (output-port? x)))

;; Equivalence predicates

(define (equal? a b)
  (cond ((eq? a b))
        ((and (pair? a)
              (pair? b))
          (and (equal? (car a) (car b))
               (equal? (cdr a) (cdr b))))
        ((string? a)
          (and (string? b)
               (string=? a b)))
        ((vector? a)
           (and (vector? b)
                (equal? (vector->list a)
                        (vector->list b))))
        (else
          (eqv? a b))))

;; List procedures

(define (list? x)
  (letrec
    ((l? (lambda (x y)
           (cond ((eq? x y) #f)
                 ((null? x) #t)
                 ((pair? x) (or (null? (cdr x))
                                (and (pair? (cdr x))
                                     (l? (cddr x) (cdr y)))))
                 (else      #f)))))
    (or (null? x)
        (and (pair? x)
             (l? (cdr x) x)))))

(define (assoc x a)
  (cond ((null? a) #f)
        ((equal? (caar a) x) (car a))
        (else (assoc x (cdr a)))))

(define (member x a)
  (cond ((null? a) #f)
        ((equal? (car a) x) a)
        (else (member x (cdr a)))))

; Auxiliary functions for FOLD-LEFT, FOLD-RIGHT, MAP

(define (map-car f a)
  (letrec
    ((mapcar1
       (lambda (a r)
         (if (null? a)
             (reverse! r)
             (mapcar1 (cdr a)
                      (cons (f (car a)) r))))))
    (mapcar1 a '())))

(define car-of
  (let ((map-car map-car))
    (lambda (a*)
      (map-car car a*))))

(define cdr-of
  (let ((map-car map-car))
    (lambda (a*)
      (map-car cdr a*))))

(define (any-null a*)
  (memq '() a*))

(define fold-left
  (let ((car-of   car-of)
        (cdr-of   cdr-of)
        (any-null any-null))
    (lambda (f b . a*)
      (letrec
        ((fold
           (lambda (a* r)
             (if (any-null a*)
                 r
                 (fold (cdr-of a*)
                       (apply f r (car-of a*)))))))
        (if (null? a*)
            (error "fold-left: too few arguments")
            (fold a* b))))))

(define fold-right
  (let ((car-of   car-of)
        (cdr-of   cdr-of)
        (any-null any-null)
        (map-car  map-car))
    (lambda (f b . a*)
      (letrec
        ((foldr
           (lambda (a* r)
             (if (any-null a*)
                 r
                 (foldr (cdr-of a*)
                        (apply f (append2 (car-of a*)
                                          (list r))))))))
        (if (null? a*)
            (error "fold-right: too few arguments")
            (foldr (map-car reverse a*) b))))))

(define append
  (let ((append2 append2))
    (letrec
      ((foldr-app
         (lambda (a)
           (cond ((null? a)
                   '())
                 ((and (pair? a)
                       (not (pair? (car a)))
                       (null? (cdr a)))
                   (car a))
                 (else
                   (append2 (car a) (foldr-app (cdr a))))))))
      (lambda a
        (foldr-app a)))))

(define (list-ref x n)
  (car (list-tail x n)))

(define map
  (let ((car-of   car-of)
        (cdr-of   cdr-of)
        (any-null any-null))
    (lambda (f . a*)
      (letrec
        ((map2
           (lambda (a* r)
             (if (any-null a*)
                 (reverse! r)
                 (map2 (cdr-of a*)
                       (cons (apply f (car-of a*))
                             r))))))
        (if (null? a*)
            (error "map: too few arguments")
            (map2 a* '()))))))

(define (for-each f . a*)
  (if (null? a*)
      (error "for-each: too few arguments")
      (apply map f a*))
  (void))

;; Arithmetic procedures

(define (expt x y)
  (letrec
    ((square
       (lambda (x) (* x x)))
     (expt2
       (lambda (x y)
         (cond ((zero? y) 1)
               ((even? y) (square (expt2 x (quotient y 2))))
               (else      (* x (square (expt2 x (quotient y 2)))))))))
    (if (negative? y)
        (error "expt: expected non-negative exponent, got" y))
    (expt2 x y)))

(define gcd
  (let ((fold-left fold-left))
    (lambda a
      (letrec
        ((gcd2
           (lambda (a b)
             (cond ((zero? b) a)
                   ((zero? a) b)
                   ((< a b) (gcd2 a (remainder b a)))
                   (else (gcd2 b (remainder a b)))))))
        (fold-left gcd2 0 (map abs a))))))

(define lcm
  (let ((fold-left fold-left))
    (lambda a
      (letrec
        ((lcm2
           (lambda (a b)
             (let ((cd (gcd a b)))
               (* cd (* (quotient a cd)
                        (quotient b cd)))))))
        (fold-left lcm2 1 (map abs a))))))

(define (modulo a b)
  (let ((rem (remainder a b)))
    (cond ((zero? rem) 0)
          ((eq? (negative? a) (negative? b)) rem)
          (else (+ b rem)))))

;; String procedures

(define (number->string n . radix)
  (letrec
    ((digits
       (string->list "0123456789abcdef"))
     (conv
       (lambda (n rdx res)
         (if (zero? n)
             (if (null? res) '(#\0) res)
             (conv (quotient n rdx)
                   rdx
                   (cons (list-ref digits (remainder n rdx))
                         res)))))
     (conv-int
       (lambda (n rdx)
         (if (negative? n)
             (list->string (cons #\- (conv (abs n) rdx '())))
             (list->string (conv n rdx '())))))
     (get-radix
       (lambda ()
         (cond ((null? radix)
                 10)
               ((<= 2 (car radix) 16)
                 (car radix))
               (else
                 (error "number->string: invalid radix"
                        (car radix)))))))
    (conv-int n (get-radix))))

(define (string->number str . radix)
  (letrec
    ((digits
       (string->list "0123456789abcdef"))
     (value-of-digit
       (lambda (x)
         (letrec
           ((v (lambda (x d n)
                 (cond ((null? d) 17)
                       ((char=? x (car d)) n)
                       (else (v x (cdr d) (+ n 1)))))))
           (v (char-downcase x) digits 0))))
     (conv3
       (lambda (lst res rdx)
         (if (null? lst)
             res
             (let ((dval (value-of-digit (car lst))))
               (and (< dval rdx)
                    (conv3 (cdr lst)
                           (+ dval (* res rdx))
                           rdx))))))
     (conv
       (lambda (lst rdx)
         (and (not (null? lst))
              (conv3 lst 0 rdx))))
     (sconv
       (lambda (lst rdx)
         (cond ((null? lst)
                 #f)
               ((char=? (car lst) #\+)
                 (conv (cdr lst) rdx))
               ((char=? (car lst) #\-)
                 (let ((r (conv (cdr lst) rdx)))
                   (if r (- r) #f)))
               (else
                 (conv lst rdx)))))
     (get-radix
       (lambda ()
         (cond ((null? radix)
                 10)
               ((<= 2 (car radix) 17)
                 (car radix))
               (else
                 (error "string->number: invalid radix" radix)))))
     (base-prefix?
       (lambda (s)
         (and (> (string-length s) 2)
              (char=? #\# (string-ref s 0))
              (memv (string-ref s 1) '(#\b #\d #\o #\x))
              #t))))
    (let ((r (if (base-prefix? str)
                 (let ((rc (string-ref str 1)))
                   (cond ((char=? rc #\b)  2)
                         ((char=? rc #\d) 10)
                         ((char=? rc #\o)  8)
                         (else            16)))
                 (get-radix)))
          (s (if (base-prefix? str)
                 (substring str 2 (string-length str))
                 str)))
      (and r (sconv (string->list s) r)))))

;; Input/output procedures

(define (newline . port)
  (apply write-char #\newline port))

(define (call-with-input-file file proc)
  (let ((f (open-input-file file)))
    (let ((r (proc f)))
      (close-input-port f)
      r)))

(define (call-with-output-file file proc)
  (let ((f (open-output-file file)))
    (let ((r (proc f)))
      (close-output-port f)
      r)))

(define with-input-from-file
  (let ((set-input-port! set-input-port!))
    (lambda (file thunk)
      (let ((outer-port (current-input-port))
            (new-port (open-input-file file)))
        (set-input-port! new-port)
        (let ((r (thunk)))
          (close-input-port new-port)
          (set-input-port! outer-port)
          r)))))

(define with-output-to-file
  (let ((set-output-port! set-output-port!))
    (lambda (file thunk)
      (let ((outer-port (current-output-port))
            (new-port (open-output-file file)))
        (set-output-port! new-port)
        (let ((r (thunk)))
          (close-output-port new-port)
          (set-output-port! outer-port)
          r)))))

;; Quasiquote Expander

(define-syntax (quasiquote tmpl)
  (letrec
    ((qq-cons
       (lambda (a b)
         (cond ((and (pair? a)
                     (eq? 'unquote-splicing (car a)))
                 (list 'append (cadr a) b))
               (else 
                 (list 'cons a b)))))
     (qq-expand-1
       (lambda (x)
         (cond ((vector? x)
                 (list 'list->vector (qq-expand-1 (vector->list x))))
               ((not (pair? x))
                 (list 'quote x))
               ((eq? 'unquote (car x))
                 (cadr x))
               ((eq? 'unquote-splicing (car x))
                 x)
               (else
                 (qq-cons (qq-expand-1 (car x))
                          (qq-expand-1 (cdr x)))))))
     (qq-expand
       (lambda (tmpl q)
         (let ((embedded-qq '()))
           (letrec
             ((extract-nested-qq
                (lambda (tmpl q)
                  (cond ((not (pair? tmpl))
                          tmpl)
                        ((or (eq? (car tmpl) 'unquote)
                             (eq? (car tmpl) 'unquote-splicing))
                          (if (not q)
                              (error
                                "quasiquote: extra unquote/unquote-splicing"))
                          (if (and (pair? (cdr tmpl))
                                   (null? (cddr tmpl)))
                              (list (car tmpl)
                                    (extract-nested-qq (cadr tmpl) #f))
                              (error (string-append
                                       (symbol->string (car tmpl))
                                       ": wrong number of arguments")
                                     tmpl)))
                        ((eq? 'quasiquote (car tmpl))
                          (if q (error "quasiquote: may not be nested"))
                          (if (and (pair? (cdr tmpl))
                                   (null? (cddr tmpl)))
                              (let ((g (gensym)))
                                (set! embedded-qq
                                      (cons (list g (qq-expand (cadr tmpl)
                                                               #t))
                                            embedded-qq))
                                g)
                              (error "quasiquote: wrong number of arguments"
                                     tmpl)))
                        (else
                          (cons (extract-nested-qq (car tmpl) q)
                                (extract-nested-qq (cdr tmpl) q)))))))
             (let ((tmpl (extract-nested-qq tmpl q)))
               (if (null? embedded-qq)
                   (qq-expand-1 tmpl)
                   (list 'let embedded-qq (qq-expand-1 tmpl)))))))))
    (qq-expand tmpl #t)))

;; Derived Syntax

; LET/LET*/LETREC helper

(define (check-bindings who b opt-arg)
  (cond ((null? b))
        ((and (pair? b)
              (pair? (car b))
              (symbol? (caar b))
              (pair? (cdar b))
              (or (null? (cddar b))
                  (and opt-arg
                       (pair? (cddar b))
                       (null? (cdddar b)))))
          (check-bindings who (cdr b) opt-arg))
        (else
          (error (string-append who ": invalid syntax") b))))

(define (split-bindings clauses)
  (letrec
    ((split3
       (lambda (clauses vars args opt)
         (cond ((null? clauses)
                 (list (reverse! vars)
                       (reverse! args)
                       (reverse! opt)))
               (else
                 (split3 (cdr clauses)
                         (cons (caar clauses) vars)
                         (cons (cadar clauses) args)
                         (if (null? (cddar clauses))
                             (cons (caar clauses) opt)
                             (cons (caddar clauses) opt))))))))
    (split3 clauses '() '() '())))

; Now that the QQ expander is here, define a
; clean version of LET (including named LET).
; Can't name it LET yet, because it uses LET.

(define-syntax %full-let
  (let ((check-bindings check-bindings)
        (split-bindings split-bindings))
    (lambda (a1 a2 . a3)
      (if (symbol? a1)
          (if (null? a3)
              (error "named let: missing body"
                     `(let ,a1 ,a2 ,@a3))
              (begin (check-bindings "let" a2 #f)
                     (let ((va (split-bindings a2)))
                       (let ((v (car va))
                             (a (cadr va)))
                         `((letrec ((,a1 (lambda ,v ,@a3)))
                             ,a1) ,@a)))))
          (begin (check-bindings "let" a1 #f)
                 (let ((va (split-bindings a1)))
                   (let ((v (car va))
                         (a (cadr va)))
                     `((lambda ,v ,a2 ,@a3) ,@a))))))))

(define-syntax let %full-let)

; Also define a clean version of LETREC.

(define-syntax %clean-letrec
  (let ((check-bindings check-bindings)
        (split-bindings split-bindings))
    (lambda (bindings expr . exprs)
      (check-bindings "letrec" bindings #f)
      (let ((va (split-bindings bindings)))
        (let ((tmps (map (lambda (x) (gensym)) bindings))
              (vars (car va))
              (args (cadr va)))
          (let ((undefineds   (map (lambda (v) (list v #f))
                                   vars))
                (tmp-bindings (map (lambda (t a) (list t a))
                                   tmps
                                   args))
                (updates      (map (lambda (v t) (list 'set! v t))
                                   vars
                                   tmps)))
            `(let ,undefineds
               (let ,tmp-bindings
                 ,@updates
                 ,expr
                 ,@exprs))))))))

(define-syntax letrec %clean-letrec)

(define-syntax let*
  (let ((check-bindings check-bindings))
    (lambda (bindings expr . exprs)
      (letrec
        ((nest-let
           (lambda (b)
             (cond ((null? b)
                     (cons expr exprs))
                   ((null? (cdr b))
                     `(let ((,(caar b) ,(cadar b)))
                        ,@(cons expr exprs)))
                   (else
                     `(let ((,(caar b) ,(cadar b)))
                        ,(nest-let (cdr b))))))))
        (check-bindings "let*" bindings #f)
        (if (null? bindings)
            `(let () ,expr ,@exprs)
            (nest-let bindings))))))

(define-syntax (case key . clauses)
  (letrec
    ((gen-clauses
       (lambda (k c*)
         (cond ((null? c*) '())
               ((or (not (pair? c*))
                    (not (pair? (car c*)))
                    (not (pair? (cdar c*))))
                 (error "case: invalid syntax" c*))
               ((null? (cdr c*))
                 (if (eq? 'else (caar c*))
                     `((else ,@(cdar c*)))
                     `(((memv ,k ',(caar c*)) ,@(cdar c*)))))
               (else
                 `(((memv ,k ',(caar c*)) ,@(cdar c*))
                     ,@(gen-clauses k (cdr c*))))))))
    (let ((k (gensym)))
      `(let ((,k ,key))
         (cond ,@(gen-clauses k clauses))))))

(define-syntax do
  (let ((check-bindings check-bindings)
        (split-bindings split-bindings))
    (lambda (var-clauses test . body)
      (if (or (not (pair? test))
              (not (list? (cdr test))))
          (error "do: invalid syntax" test))
      (check-bindings "do" var-clauses #t)
      (let ((loop (gensym))
            (var+init+step (split-bindings var-clauses)))
        (let ((v (car   var+init+step))
              (i (cadr  var+init+step))
              (s (caddr var+init+step)))
          `(letrec
             ((,loop
                (lambda ,v
                  (if ,(car test)
                      (begin ,@(cdr test))
                      (begin ,@body (,loop ,@s))))))
             (,loop ,@i)))))))

(define-syntax (delay expr)
  `(let ((value #f))
     (lambda ()
       (if value
           (car value)
           (let ((x ,expr))
             (if value
                 (car value)
                 (begin (set! value (list x))
                        (car value))))))))

(define (force x) (x))

;; Utilities

(define (print . x*)
  (letrec
    ((p (lambda (x* first)
          (cond ((not (null? x*))
                  (if (not first) (write-char #\space))
                  (write (car x*))
                  (p (cdr x*) #f))))))
    (p x* #t)
    (newline)))

(define (locate-file file)
  (letrec
    ((split
       (lambda (s)
         (let loop ((in  (string->list s))
                    (tmp '())
                    (out '()))
           (cond ((null? in)
                   (if (null? tmp)
                       out
                       (reverse! (cons (list->string (reverse! tmp))
                                       out))))
                 ((char=? #\: (car in))
                   (loop (cdr in)
                         '()
                         (cons (list->string (reverse! tmp))
                               out)))
                 (else
                   (loop (cdr in)
                         (cons (car in) tmp)
                         out)))))))
    (let loop ((path (split *library-path*)))
      (and (not (null? path))
           (let ((full-path (string-append (car path) "/" file)))
             (if (file-exists? full-path)
                 full-path
                 (loop (cdr path))))))))

(define load-from-library 
  (let ((locate-file locate-file))
    (lambda (file)
      (let ((full-path (locate-file file))
            (do-load (lambda (file)
                       (begin (if (not *loading*)
                                  (begin (display "; loading from ")
                                         (display file)
                                         (newline)))
                              (load file)))))
        (if full-path
            (do-load full-path)
            (let ((full-path (locate-file (string-append file ".scm"))))
              (if full-path
                  (do-load full-path)
                  (error "cannot locate file" file))))))))

(define-syntax (require-extension . x*)
  (do ((x* x* (cdr x*))
       (na '()))
      ((null? x*)
        (if (not (null? na))
            (error "extension(s) required, but not compiled-in"
                   (reverse! na))))
    (if (not (memq (car x*) *extensions*))
        (set! na (cons (car x*) na)))))

