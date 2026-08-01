;; delay / force (R7RS 4.2.5)

(check-assert "promise?" (promise? (delay 1)))
(check-assert "promise? number" (not (promise? 1)))
(check-eqv "force delay" 42 (force (delay 42)))
(check-eqv "force non-promise" 9 (force 9))

(define p (delay (+ 20 22)))
(check-eqv "force once" 42 (force p))
(check-eqv "force again" 42 (force p))

(define count 0)
(define once
  (delay
    (begin
      (set! count (+ count 1))
      count)))
(check-eqv "lazy side-effect once" 1 (force once))
(check-eqv "memoized" 1 (force once))
(check-eqv "count stays 1" 1 count)

(check-eqv "make-promise" 3 (force (make-promise 3)))

(check-finish)
