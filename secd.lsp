;;;
;;; secd.lsp : SECD 仮想マシンによる Scheme コンパイラ
;;;
;;;            (1) 基本機能の実装
;;;            (2) 伝統的なマクロの実装
;;;            (3) 継続の実装
;;;            (4) 末尾再帰最適化
;;;
;;;            Copyright (C) 2009-2021 Makoto Hiroi
;;;

;;; 関数宣言
(declaim (ftype (function (t list list t) t) comp))
(declaim (ftype (function (list list list list) t) vm))

;;; 大域変数
(defvar *global-environment* nil)

;;; 大域変数の値を求める
(defun get-gvar (sym)
  (let ((val (assoc sym *global-environment*)))
    (if val
        (cdr val)
      (error "unbound variable ~S" sym))))

;;; 大域変数の値を書き換える
(defun set-gvar (sym val)
  (let ((cell (assoc sym *global-environment*)))
    (if cell
        (rplacd cell val)
      (error "unbound variable ~S" sym))))

;;; 変数の位置を求める
(defun position-var (sym ls)
  (labels ((iter (i ls)
             (cond ((null ls) nil)
                   ((symbolp ls)
                    (if (eq sym ls) (- (1+ i)) nil))
                   ((eq sym (car ls)) i)
                   (t (iter (1+ i) (cdr ls))))))
    (iter 0 ls)))

;;; フレームと変数の位置を求める
(defun location (sym ls)
  (labels ((iter (i ls)
             (if (null ls)
                 nil
               (let ((j (position-var sym (car ls))))
                 (if j
                     (cons i j)
                   (iter (1+ i) (cdr ls)))))))
    (iter 0 ls)))

;;; 自己評価フォームか
(defun self-evaluation-p (expr)
  (and (atom expr) (not (symbolp expr))))

;;; マクロか
(defun macro-p (expr)
  (let ((val (assoc expr *global-environment*)))
    (and val (consp (cdr val)) (eq 'macro (cadr val)))))

;;; マクロのコードを取り出す
(defun get-macro-code (expr)
  (caddr (get-gvar expr)))

;;; S 式をコンパイルする
(defun compile-expr (expr)
  (comp expr '() '(stop) nil))

;;; body のコンパイル
(defun comp-body (body env code)
  (if (null (cdr body))
      (comp (car body) env code t)
    (comp (car body)
          env
          (list* 'pop
                 (comp-body (cdr body) env code))
          nil)))

;;; 引数を評価するコードを生成する
(defun complis (expr env code)
  (if (null expr)
      code
    (comp (car expr) env (complis (cdr expr) env code) nil)))

;;; コンパイル本体
(defun comp (expr env code tail)
  (cond ((self-evaluation-p expr)
         (list* 'ldc expr code))
        ((symbolp expr)
         (let ((pos (location expr env)))
           (if pos
               ;; 局所変数
               (list* 'ld pos code)
             ;; 大域変数
             (list* 'ldg expr code))))
        ((eq (car expr) 'quote)
         (list* 'ldc (cadr expr) code))
        ((eq (car expr) 'if)
         (if tail
             ;; 末尾呼び出し
             (let ((t-clause (comp (caddr expr) env '(rtn) t))
                   (f-clause
                    (if (null (cdddr expr))
                        (list 'ldc '*undef* 'join)
                      (comp (cadddr expr) env '(rtn) t))))
               (comp (cadr expr) env (list* 'selr t-clause f-clause (cdr code)) nil))
           (let ((t-clause (comp (caddr expr) env '(join) nil))
                 (f-clause
                  (if (null (cdddr expr))
                      (list 'ldc '*undef* 'join)
                    (comp (cadddr expr) env '(join) nil))))
             (comp (cadr expr) env (list* 'sel t-clause f-clause code) nil))))
        ((eq (car expr) 'lambda)
         (let ((body (comp-body (cddr expr) (cons (cadr expr) env) '(rtn))))
           (list* 'ldf body code)))
        ((eq (car expr) 'define)
         (comp (caddr expr) env (list* 'def (cadr expr) code) nil))
        ((eq (car expr) 'define-macro)
         (comp (caddr expr) env (list* 'defm (cadr expr) code) nil))
        ((eq (car expr) 'set!)
         (let ((pos (location (cadr expr) env)))
           (if pos
               ;; 局所変数
               (comp (caddr expr) env (list* 'lset pos code) nil)
             ;; 大域変数
             (comp (caddr expr) env (list* 'gset (cadr expr) code) nil))))
        ((eq (car expr) 'call/cc)
         (list* 'ldct code 'args 1 (comp (cadr expr) env (cons 'app code) nil)))
        ((eq (car expr) 'apply)
         (complis (cddr expr)
                  env
                  (list* 'args-ap
                         (length (cddr expr))
                         (comp (cadr expr) env (cons 'app code) nil))))
        ((macro-p (car expr))
         ;; マクロ展開してからコンパイルする
         (let ((new-expr (vm '()
                             (list (cdr expr))
                             (get-macro-code (car expr))
                             (list (list '() '() '(stop))))))
           (comp new-expr env code nil)))
        (t  ; 関数呼び出し
         (complis (cdr expr)
                  env
                  (list* 'args
                         (length (cdr expr))
                         (comp (car expr) env (cons (if tail 'tapp 'app) code) nil))))))

;;;
;;; 仮想マシン
;;;

;;; 局所変数の値を求める
(defun get-lvar (e i j)
  (if (<= 0 j)
      (nth j (nth i e))
    (nthcdr (- (1+ j)) (nth i e))))

;;; 局所変数の値を更新する
(defun set-lvar (e i j val)
  (if (<= 0 j)
      (setf (nth j (nth i e)) val)
    (if (= j -1)
        (rplaca (nthcdr i e) val)
      (rplacd (nthcdr (- (+ j 2)) (nth i e)) val))))

;;; 仮想マシンでコードを実行する
(defun vm (s e c d)
  (loop
    (case (pop c)
      ((ld)
       (let ((pos (pop c)))
         (push (get-lvar e (car pos) (cdr pos)) s)))
      ((ldc)
       (push (pop c) s))
      ((ldg)
       (push (get-gvar (pop c)) s))
      ((ldf)
       (push (list 'closure (pop c) e) s))
      ((ldct)
       (push (list 'continuation s e (pop c) d) s))
      ((lset)
       (let ((pos (pop c)))
         (set-lvar e (car pos) (cdr pos) (car s))))
      ((gset)
       (set-gvar (pop c) (car s)))
      ((app)
       (let ((clo (pop s)) (lvar (pop s)))
         (case (pop clo)
           ((primitive)
            (push (apply (car clo) lvar) s))
           ((continuation)
            (setq s (cons (car lvar) (car clo))
                  e (cadr clo)
                  c (caddr clo)
                  d (cadddr clo)))
           (t
            (push (list s e c) d)
            (setq s nil
                  e (cons lvar (cadr clo))
                  c (car clo))))))
      ((tapp)
       (let ((clo (pop s)) (lvar (pop s)))
         (case (pop clo)
           ((primitive)
            (push (apply (car clo) lvar) s))
           ((continuation)
            (setq s (cons (car lvar) (car clo))
                  e (cadr clo)
                  c (caddr clo)
                  d (cadddr clo)))
           (t
            (setq e (cons lvar (cadr clo))
                  c (car clo))))))
      ((rtn)
       (let ((save (pop d)))
         (setq s (cons (car s) (car save))
               e (cadr save)
               c (caddr save))))
      ((sel)
       (let ((t-clause (pop c))
             (e-clause (pop c)))
         (push c d)
         (setq c (if (eq (pop s) 'false) e-clause t-clause))))
      ((selr)
       (let ((t-clause (pop c))
             (e-clause (pop c)))
         (setq c (if (eq (pop s) 'false) e-clause t-clause))))
      ((join)
       (setq c (pop d)))
      ((pop) (pop s))
      ((args)
       (let ((a nil))
         (dotimes (n (pop c) (push a s))
           (push (pop s) a))))
      ((args-ap)
       (let ((a (copy-list (pop s))))
         (dotimes (n (1- (pop c)) (push a s))
           (push (pop s) a))))
      ((def)
       (let ((sym (pop c)))
         (push (cons sym (pop s)) *global-environment*)
         (push sym s)))
      ((defm)
       (let ((sym (pop c)))
         (push (cons sym (cons 'macro (pop s))) *global-environment*)
         (push sym s)))
      ((stop) (return (car s)))
      (t (error "unknown opcode")))))

;;; 大域変数
(setq *global-environment*
      (list
       (cons 'true  'true)
       (cons 'false 'false)
       (cons 'nil   'nil)
       (cons 'quit  'quit)
       (list 'car   'primitive (lambda (x)
                                 (if (null x)
                                     (error "type error -- car NIL")
                                   (car x))))
       (list 'cdr   'primitive (lambda (x)
                                 (if (null x)
                                     (error "type error -- cdr NIL")
                                   (cdr x))))
       (list 'cons  'primitive #'cons)
       (list 'eq?   'primitive (lambda (x y) (if (eq x y) 'true 'false)))
       (list 'eqv?  'primitive (lambda (x y) (if (eql x y) 'true 'false)))
       (list 'pair? 'primitive (lambda (x) (if (consp x) 'true 'false)))
       (list 'display 'primitive (lambda (x) (princ x) '*undef*))
       (list 'newline 'primitive (lambda () (terpri) '*undef*))
       (list '+     'primitive #'+)
       (list '-     'primitive #'-)
       (list '*     'primitive #'*)
       (list '/     'primitive #'/)
       (list '=     'primitive (lambda (&rest x) (if (apply #'= x) 'true 'false)))
       (list '<     'primitive (lambda (&rest x) (if (apply #'< x) 'true 'false)))
       (list '>     'primitive (lambda (&rest x) (if (apply #'> x) 'true 'false)))
       (list '<=    'primitive (lambda (&rest x) (if (apply #'<= x) 'true 'false)))
       (list '>=    'primitive (lambda (&rest x) (if (apply #'>= x) 'true 'false)))
       ))

;;;
;;; read-eval-print-loop
;;;
(defun change-readtable ()
  (set-macro-character
   #\`
   (lambda (stream char)
     (declare (ignore char))
     (list 'backquote (read stream t nil t))))
  (set-macro-character
   #\,
   (lambda (stream char)
     (declare (ignore char))
     (cond ((char= (peek-char nil stream) #\@)
            (read-char stream)
            (list 'splice (read stream t nil t)))
           (t (list 'unquote (read stream t nil t)))))))

(defun repl (&rest file-list)
  (unwind-protect
      (progn
        (change-readtable)
        (dolist (file file-list)
          (with-open-file (in file :direction :input)
            (do ((output t))
                ((eq output nil) (terpri))
              (setf output (vm '() '() (compile-expr (read in nil)) '()))
              (print output))))
        (do ((output nil))
            ((eq output 'quit))
          (princ ">>> ")
          (force-output)
          (handler-case
              (let ((expr (compile-expr (read))))
                ;; (time (setf output (vm '() '() expr '())))
                (setf output (vm '() '() expr '()))
                (princ output)
                (terpri))
            (simple-error (c) (format t "ERROR: ~a~%" c)))))
    (setq *readtable* (copy-readtable nil))))
