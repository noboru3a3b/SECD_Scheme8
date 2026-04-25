;
; mlib.scm : micro Scheme 用ライブラリ
;
;            Copyright (C) 2009 Makoto Hiroi
;

;;; リスト操作関数

(define caar (lambda (x) (car (car x))))
(define cdar (lambda (x) (cdr (car x))))
(define cadr (lambda (x) (car (cdr x))))
(define cddr (lambda (x) (cdr (cdr x))))
;; add  2013-11-13 okada-n
(define caaar (lambda (x) (car (car (car x)))))
(define cdaar (lambda (x) (cdr (car (car x)))))
(define cadar (lambda (x) (car (cdr (car x)))))
(define cddar (lambda (x) (cdr (cdr (car x)))))
(define caadr (lambda (x) (car (car (cdr x)))))
(define cdadr (lambda (x) (cdr (car (cdr x)))))
(define caddr (lambda (x) (car (cdr (cdr x)))))
(define cdddr (lambda (x) (cdr (cdr (cdr x)))))

; リストの結合
(define append
  (lambda (xs ys)
    (if (null? xs)
        ys
      (cons (car xs) (append (cdr xs) ys)))))

; リストの探索
(define memq
  (lambda (x ls)
    (if (null? ls)
        false
        (if (eq? x (car ls))
            ls
          (memq x (cdr ls))))))

;
(define memv
  (lambda (x ls)
    (if (null? ls)
        false
        (if (eqv? x (car ls))
            ls
          (memv x (cdr ls))))))

; 連想リストの探索
(define assq
  (lambda (x ls)
    (if (null? ls)
        false
      (if (eq? x (car (car ls)))
          (car ls)
        (assq x (cdr ls))))))

;
(define assv
  (lambda (x ls)
    (if (null? ls)
        false
      (if (eqv? x (car (car ls)))
          (car ls)
        (assv x (cdr ls))))))

;;; 高階関数

; マップ
(define map
  (lambda (fn ls)
    (if (null? ls)
        '()
      (cons (fn (car ls)) (map fn (cdr ls))))))

;
(define map-2
  (lambda (fn xs ys)
    (if (null? xs)
        '()
      (cons (fn (car xs) (car ys)) (map-2 fn (cdr xs) (cdr ys))))))

; フィルター
(define filter
  (lambda (fn ls)
    (if (null? ls)
        '()
      (if (fn (car ls))
          (cons (car ls) (filter fn (cdr ls)))
        (filter fn (cdr ls))))))

; 畳み込み
(define fold-right
  (lambda (fn a ls)
    (if (null? ls)
        a
      (fn (car ls) (fold-right fn a (cdr ls))))))

;
(define fold-left
  (lambda (fn a ls)
    (if (null? ls)
        a
      (fold-left fn (fn a (car ls)) (cdr ls)))))

;;; マクロ

; quasiquote
(define transfer
  (lambda (ls)
    (if (pair? ls)
        (if (pair? (car ls))
            (if (eq? (caar ls) 'unquote)
                (list 'cons (cadar ls) (transfer (cdr ls)))
              (if (eq? (caar ls) 'splice)
                  (list 'append (cadar ls) (transfer (cdr ls)))
                (list 'cons (transfer (car ls)) (transfer (cdr ls)))))
          (list 'cons (list 'quote (car ls)) (transfer (cdr ls))))
      (list 'quote ls))))

(define-macro backquote (lambda (x) (transfer x)))

; let
(define-macro let
  (lambda (args . body)
    (if (pair? args)
        `((lambda ,(map car args) ,@body) ,@(map cadr args))
      ; named-let
      `(letrec ((,args (lambda ,(map car (car body)) ,@(cdr body))))
        (,args ,@(map cadr (car body)))))))

; and
(define-macro and
  (lambda args
    (if (null? args)
        true
      (if (null? (cdr args))
          (car args)
        `(if ,(car args) (and ,@(cdr args)) false)))))

; or
(define-macro or
  (lambda args
    (if (null? args)
        false
      (if (null? (cdr args))
          (car args)
        (let ((value (gensym)))
          `(let ((,value ,(car args)))
             (if ,value ,value (or ,@(cdr args)))))))))

; let*
(define-macro let*
  (lambda (args . body) 
    (if (null? (cdr args))
        `(let (,(car args)) ,@body)
      `(let (,(car args)) (let* ,(cdr args) ,@body)))))

; letrec
(define-macro letrec
  (lambda (args . body)
    (let ((vars (map car args))
          (vals (map cadr args)))
      `(let ,(map (lambda (x) `(,x ':undef)) vars)  ; okada-n
            ,@(map-2 (lambda (x y) `(set! ,x ,y)) vars vals)
            ,@body))))

; begin
(define-macro begin
  (lambda args
    (if (null? args)
        `((lambda () ':undef))  ; okada-n
      `((lambda () ,@args)))))


; cond
(define-macro cond
  (lambda args
    (if (null? args)
        '':undef  ; change for debug 2011-05-29 okada-n
      (if (eq? (caar args) 'else)
          `(begin ,@(cdar args))
        (if (null? (cdar args))
            (let ((value (gensym)))
              `(let ((,value ,(caar args)))
                 (if ,value ,value (cond ,@(cdr args)))))
          `(if ,(caar args)
               (begin ,@(cdar args))
               (cond ,@(cdr args))))))))

; case
(define-macro case
  (lambda (key . args)
    (if (null? args)
        '':undef  ; okada-n
      (if (eq? (caar args) 'else)
          `(begin ,@(cdar args))
        `(if (memv ,key ',(caar args))
             (begin ,@(cdar args))
           (case ,key ,@(cdr args)))))))

; do
(define-macro do
  (lambda (var-form test-form . args)
    (let ((vars (map car var-form))
          (vals (map cadr var-form))
          (step (map cddr var-form)))
      `(letrec ((loop (lambda ,vars
                              (if ,(car test-form)
                                  (begin ,@(cdr test-form))
                                (begin
                                  ,@args
                                  (loop ,@(map-2 (lambda (x y)
                                                   (if (null? x) y (car x)))
                                                 step
                                                 vars)))))))
        (loop ,@vals)))))

;;; マクロを使った関数の定義

; reverse
(define reverse
  (lambda (ls)
    (letrec ((iter (lambda (ls a)
                     (if (null? ls)
                         a
                       (iter (cdr ls) (cons (car ls) a))))))
      (iter ls '()))))

; イテレータを生成する関数
(define make-iter
 (lambda (proc . args)
  (letrec ((iter
            (lambda (return)
              (apply 
                proc
                (lambda (x)             ; 高階関数に渡す関数の本体
                  (set! return          ; 脱出先継続の書き換え
                   (call/cc
                    (lambda (cont)
                      (set! iter cont)  ; 継続の書き換え
                      (return x)))))
                args)
                ; 終了後は継続 return で脱出
                (return false))))
    (lambda ()
      (call/cc
        (lambda (cont) (iter cont)))))))

; 木の高階関数
(define for-each-tree
 (lambda (fn ls)
  (let loop ((ls ls))
    (cond ((null? ls) '())
          ((pair? ls)
           (loop (car ls))
           (loop (cdr ls)))
          (else (fn ls))))))

; delay と force
(define-macro delay 
  (lambda (expr)
    `(make-promise (lambda () ,expr))))

(define make-promise
  (lambda (f)
    (let ((flag false) (result false))
      (lambda ()
        (if (not flag)
            (let ((x (f)))
              (if (not flag)
                  (begin (set! flag true)
                         (set! result x)))))
        result))))

(define force 
  (lambda (promise) (promise)))

;; fact
;; usage: (fact 10 1)
(define fact
  (lambda (n a)
    (if (= n 0)
        a
      (fact (- n 1) (* a n)))))

;; tarai と tak
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

;;; queue & primes ;;;

;; queue
(define make-queue (lambda (x)
  (let ((seed (cons x '())))
    (cons seed seed))))

(define en-queue! (lambda (queue x)
  (let ((q (cons x '())))
    (set-cdr! (cdr queue) q)
    (set-cdr! queue q)
    queue)))

(define de-queue! (lambda (queue)
  (let* ((head (car queue))
         (lst (cdar queue))
         (val (car lst))
         (rest (cdr lst)))
    (set-cdr! head rest)
    (if (null? rest) (set-cdr! queue head))
    val)))

(define get-queue-lst (lambda (queue) (cdar queue)))

(define get-all-lst (lambda (queue) (car queue)))

;; primes
(define primes (lambda (queue x xmax)
  (cond ((> x xmax)
	  (get-all-lst queue))
	((is-prime (get-queue-lst queue) x)
	  (primes (en-queue! queue x) (+ x 2) xmax))
	(else
	  (primes queue (+ x 2) xmax)))))

(define is-prime (lambda (p-lst x)
  (if (null? p-lst)
      true
      (let ((p (car p-lst)))
	(cond ((> (* p p) x)
	              true)
	            ((= 0 (modulo x p))
		            false)
		          (else
			          (is-prime (cdr p-lst) x)))))))

;;; file i/o ;;;

;; cr-conv
(define cr-conv (lambda (from to)
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

;; cr-cut
(define cr-cut (lambda (from to)
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

;; copy-file
(define copy-file (lambda (from to)
  (let ((pfr (open-input-file from))
        (pto (open-output-file to)))
    (let loop((c (read-char pfr)))
      (if (eof-object? c)
          (begin
            (close-input-port pfr)
            (close-output-port pto))
          (begin
            (write-char c pto)
            (loop (read-char pfr))))))))

