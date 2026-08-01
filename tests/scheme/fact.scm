; Smoke Scheme file for manual runs
(define (fact n)
  (if (= n 0)
      1
      (* n (fact (- n 1)))))
(display (fact 5))
(newline)
