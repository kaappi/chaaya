;; Eval / environment / load / time

(check-eqv "eval interaction-environment"
           42
           (eval '(+ 40 2) (interaction-environment)))

(define env-base (environment '(scheme base)))
(check-eqv "environment import set"
           9
           (eval '(+ 4 5) env-base))

(define env-null (null-environment 5))
(check-eqv "null-environment syntax form"
           7
           (eval '(if #t 7 8) env-null))

(define env-r5rs (scheme-report-environment 5))
(check-eqv "scheme-report-environment"
           15
           (eval '(+ 7 8) env-r5rs))

(define load-path ".bootstrap_tmp_eval_load.scm")
(when (file-exists? load-path)
  (delete-file load-path))

(with-output-to-file load-path
  (lambda ()
    (display "(define phase7a-loaded 123)")
    (newline)
    (display "(+ phase7a-loaded 1)")
    (newline)))

(check-eqv "load returns last value" 124 (load load-path))
(check-eqv "load defines global" 123 phase7a-loaded)
(delete-file load-path)

(define t (make-time 'time-utc 500 11))
(check-assert "time? true" (time? t))
(check-eq "time-type symbol" 'time-utc (time-type t))
(check-eqv "time-second accessor" 11 (time-second t))
(check-eqv "time-nanosecond accessor" 500 (time-nanosecond t))
(check-assert "current-second number" (number? (current-second)))
(check-assert "current-jiffy integer" (exact-integer? (current-jiffy)))
(check-eqv "jiffies-per-second value" 1000000000 (jiffies-per-second))

(check-finish)
