;; case-lambda (R7RS 4.2.3)

(define cl
  (case-lambda
    (() 0)
    ((x) x)
    ((x y) (+ x y))
    ((x y . rest) (+ x y (length rest)))))

(check-eqv "case-lambda 0" 0 (cl))
(check-eqv "case-lambda 1" 7 (cl 7))
(check-eqv "case-lambda 2" 5 (cl 2 3))
(check-eqv "case-lambda rest" 6 (cl 1 2 3 4 5))

(check-finish)
