;; Phase 4 — call/cc, dynamic-wind, exceptions (bootstrap).

(check-eqv "call/cc escape"
           42
           (call/cc (lambda (k) (k 42) 99)))

(check-eqv "call/cc unused"
           7
           (call/cc (lambda (k) (+ 3 4))))

(check-eqv "call/cc alias"
           1
           (call-with-current-continuation (lambda (k) (k 1))))

(define *order* '())
(define (note! x)
  (set! *order* (cons x *order*))
  x)

(check-eqv "dynamic-wind value"
           5
           (dynamic-wind
             (lambda () (note! 'b))
             (lambda () (note! 't) 5)
             (lambda () (note! 'a))))

(check-equal "dynamic-wind order"
             '(a t b)
             *order*)

(set! *order* '())
(check-eqv "dynamic-wind + call/cc"
           10
           (dynamic-wind
             (lambda () (note! 'before))
             (lambda ()
               (call/cc (lambda (k)
                          (note! 'escape)
                          (k 10)
                          (note! 'dead))))
             (lambda () (note! 'after))))

(check-equal "wind after on escape"
             '(after escape before)
             *order*)

(check-eqv "with-exception-handler"
           'handled
           (call/cc
            (lambda (k)
              (with-exception-handler
                (lambda (e) (k 'handled))
                (lambda () (raise 'boom) 'nope)))))

(check-eqv "raise-continuable"
           42
           (with-exception-handler
             (lambda (e) 42)
             (lambda () (raise-continuable 'x))))

(check-finish)
