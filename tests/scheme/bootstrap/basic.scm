;; Adapted from kaappi/tests/scheme/smoke/basic.scm for Chaaya bootstrap
;; (no import / SRFI-64 — uses harness.scm).

(check-eqv "addition" 3 (+ 1 2))
(check-eqv "subtraction" 7 (- 10 3))
(check-eqv "multiplication" 20 (* 4 5))

(check-eq "if true" 'yes (if #t 'yes 'no))
(check-eq "if false" 'no (if #f 'yes 'no))

(check-eqv "variadic +" 15 (+ 1 2 3 4 5))

(define x 42)
(check-eqv "define x" 42 x)
(set! x 99)
(check-eqv "set! x" 99 x)

(define add1 (lambda (x) (+ x 1)))
(check-eqv "lambda add1" 11 (add1 10))
(check-eqv "anonymous lambda" 7 ((lambda (x y) (+ x y)) 3 4))

(check-eqv "begin" 3 (begin 1 2 3))

(check-equal "quote" '(a b c) (quote (a b c)))

(check-equal "cons" '(1 . 2) (cons 1 2))
(check-eqv "car" 1 (car (cons 1 2)))
(check-eqv "cdr" 2 (cdr (cons 1 2)))

(check-eqv "null? empty" #t (null? '()))
(check-eqv "null? non-empty" #f (null? 42))
(check-eqv "pair? pair" #t (pair? (cons 1 2)))
(check-eqv "pair? non-pair" #f (pair? 42))

(check-eqv "nested arithmetic" 12 (+ (* 2 3) (- 10 4)))

(check-finish)
