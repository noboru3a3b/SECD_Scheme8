;
; mlib8.scm : micro Scheme support library
;
;            Copyright (C) 2009 Makoto Hiroi
;

;;; Basic list helpers

;; add 2013-11-13 okada-n
(define caaar (lambda (x) (car (car (car x)))))
(define cdaar (lambda (x) (cdr (car (car x)))))
(define cadar (lambda (x) (car (cdr (car x)))))
(define cddar (lambda (x) (cdr (cdr (car x)))))
(define caadr (lambda (x) (car (car (cdr x)))))
(define cdadr (lambda (x) (cdr (car (cdr x)))))
(define cdddr (lambda (x) (cdr (cdr (cdr x)))))

(define memv
  (lambda (x ls)
    (if (null? ls)
        false
        (if (eqv? x (car ls))
            ls
            (memv x (cdr ls))))))

(define assv
  (lambda (x ls)
    (if (null? ls)
        false
        (if (eqv? x (car (car ls)))
            (car ls)
            (assv x (cdr ls))))))

;;; Higher-order list functions (Scheme definitions to allow proper
;;; continuation escape from within map/filter/for-each/fold bodies).
;;; These override the C++ PRIMITIVE versions so that a call/cc escape
;;; inside the callback propagates correctly through the recursive calls
;;; rather than being trapped by a C++ loop frame.

(define map
  (lambda (fn ls)
    (if (null? ls)
        '()
        (cons (fn (car ls)) (map fn (cdr ls))))))

(define filter
  (lambda (fn ls)
    (if (null? ls)
        '()
        (if (fn (car ls))
            (cons (car ls) (filter fn (cdr ls)))
            (filter fn (cdr ls))))))

(define for-each
  (lambda (fn ls)
    (if (null? ls)
        '()
        (begin
          (fn (car ls))
          (for-each fn (cdr ls))))))

(define fold-right
  (lambda (fn a ls)
    (if (null? ls)
        a
        (fn (car ls) (fold-right fn a (cdr ls))))))

(define fold-left
  (lambda (fn a ls)
    (if (null? ls)
        a
        (fold-left fn (fn a (car ls)) (cdr ls)))))

;;; Iterator helpers

(define map-2
  (lambda (fn xs ys)
    (if (null? xs)
        '()
        (cons (fn (car xs) (car ys)) (map-2 fn (cdr xs) (cdr ys))))))

;;; mlib9 for micro_scheme11.cpp
;
; NOTE:
;   micro_scheme11.cpp already has built-in handling for these core forms:
;   backquote, let, let*, letrec, begin, and, or, cond, case, do.
;   Redefining them here as macros can change evaluation route and break
;   VM behavior parity. So mlib9 intentionally does NOT redefine them.

;;; Macro utility functions

;; Iterator via call/cc (mlib7 style).
(define make-iter
 (lambda (proc . args)
  (letrec ((iter
            (lambda (return)
              (apply
                proc
                (lambda (x)
                  (set! return
                   (call/cc
                    (lambda (cont)
                      (set! iter cont)
                      (return x)))))
                args)
              (return false))))
    (lambda ()
      (call/cc
        (lambda (cont) (iter cont)))))))

(define for-each-tree
  (lambda (fn ls)
    (let loop ((ls ls))
      (cond ((null? ls) '())
            ((pair? ls)
             (loop (car ls))
             (loop (cdr ls)))
            (else (fn ls))))))

;; delay and force
(define-macro delay
  (lambda (expr)
    `(make-promise (lambda () ,expr))))

(define make-promise
  (lambda (f)
    (let ((state (cons false false)))
      (lambda ()
        (if (not (car state))
            (begin
              (set-car! state true)
              (set-cdr! state (f))))
        (cdr state)))))

(define force
  (lambda (promise) (promise)))

;; fact
;; usage: (fact 10 1)
(define fact
  (lambda (n a)
    (if (= n 0)
        a
        (fact (- n 1) (* a n)))))

;; tarai and tak
;; usage: (tarai 10 5 0)
;;        (tak 14 7 0)
(define tarai
  (lambda (x y z)
    (if (<= x y)
        y
        (tarai (tarai (- x 1) y z) (tarai (- y 1) z x) (tarai (- z 1) x y)))))

(define tak
  (lambda (x y z)
    (if (<= x y)
        z
        (tak (tak (- x 1) y z) (tak (- y 1) z x) (tak (- z 1) x y)))))

;; tarai (delay)
;; usage: (tarai-delay 80 40 (delay 0))
(define tarai-delay
  (lambda (x y z)
    (if (<= x y)
        y
        (let ((zz (force z)))
          (tarai-delay (tarai-delay (- x 1) y (delay zz))
                       (tarai-delay (- y 1) zz (delay x))
                       (delay (tarai-delay (- zz 1) x (delay y))))))))

;;; queue and primes

(define make-queue
  (lambda (x)
    (let ((seed (cons x '())))
      (cons seed seed))))

(define en-queue!
  (lambda (queue x)
    (let ((q (cons x '())))
      (set-cdr! (cdr queue) q)
      (set-cdr! queue q)
      queue)))

(define de-queue!
  (lambda (queue)
    (let* ((head (car queue))
           (lst (cdar queue))
           (val (car lst))
           (rest (cdr lst)))
      (set-cdr! head rest)
      (if (null? rest) (set-cdr! queue head))
      val)))

(define get-queue-lst (lambda (queue) (cdar queue)))
(define get-all-lst (lambda (queue) (car queue)))

(define primes
  (lambda (queue x xmax)
    (cond ((> x xmax)
           (get-all-lst queue))
          ((is-prime (get-queue-lst queue) x)
           (primes (en-queue! queue x) (+ x 2) xmax))
          (else
           (primes queue (+ x 2) xmax)))))

(define is-prime
  (lambda (p-lst x)
    (if (null? p-lst)
        true
        (let ((p (car p-lst)))
          (cond ((> (* p p) x)
                 true)
                ((= 0 (modulo x p))
                 false)
                (else
                 (is-prime (cdr p-lst) x)))))))

;;; file i/o

(define cr-conv
  (lambda (from to)
    (let ((pfr (open-input-file from))
          (pto (open-output-file to)))
      (let loop ((line (read-line pfr)))
        (if (eof-object? line)
            (begin
              (close-input-port pfr)
              (close-output-port pto))
            (begin
              (write line pto)
              (write_newline pto)
              (loop (read-line pfr))))))))

(define cr-cut
  (lambda (from to)
    (let ((pfr (open-input-file from))
          (pto (open-output-file to)))
      (let loop ((line (read-line pfr)))
        (if (eof-object? line)
            (begin
              (close-input-port pfr)
              (close-output-port pto))
            (begin
              (write line pto)
              (loop (read-line pfr))))))))

(define copy-file
  (lambda (from to)
    (let ((pfr (open-input-file from))
          (pto (open-output-file to)))
      (let loop ((c (read-char pfr)))
        (if (eof-object? c)
            (begin
              (close-input-port pfr)
              (close-output-port pto))
            (begin
              (write-char c pto)
              (loop (read-char pfr))))))))
