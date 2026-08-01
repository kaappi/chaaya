;; define-values, let-values, let*-values, delay-force (R7RS §4.2.2, §5.3.3)

(define-values (dv-a dv-b) (values 1 2))
(check-equal "define-values fixed" '(1 2) (list dv-a dv-b))

(define-values (dv-x . dv-rest) (values 10 20 30))
(check-equal "define-values dotted" '(10 (20 30)) (list dv-x dv-rest))

(define-values dv-all (values 4 5 6))
(check-equal "define-values single-var" '(4 5 6) dv-all)

(check-equal "define-values in lambda"
  3
  ((lambda ()
     (define-values (ia ib) (values 1 2))
     (+ ia ib))))

(check-equal "let-values basic" 3
  (let-values (((a b) (values 1 2))) (+ a b)))

(check-equal "let-values parallel scoping" 100
  (let ((x 100))
    (let-values (((x) (values 1)) ((b) (values x)))
      b)))

(check-equal "let-values dotted" '(1 (2 3))
  (let-values (((a . r) (values 1 2 3))) (list a r)))

(check-equal "let*-values sequential" 3
  (let*-values (((a b) (values 1 2))
                ((c) (values (+ a b))))
    c))

(check-equal "let*-values shadow" 2
  (let*-values (((x) (values 2))
                ((y) (values x)))
    y))

(check-equal "let*-values empty internal define" 1
  (let ((x 1))
    (let*-values ()
      (define x 2)
      #f)
    x))

(define (df-chain n)
  (if (= n 0)
      (delay 'end)
      (delay-force (df-chain (- n 1)))))
(check-equal "delay-force chain" 'end (force (df-chain 5)))

(check-finish)
