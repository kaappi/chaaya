;; Bootstrap test harness for Chaaya.
;; Prepended to each tests/scheme/bootstrap/*.scm by the runner.
;; Adapted from Kaappi's SRFI-64 style without requiring import/macros.

(define *fail-count* 0)
(define *pass-count* 0)

(define (check-equal name expected actual)
  (if (equal? expected actual)
      (begin
        (set! *pass-count* (+ *pass-count* 1))
        (display "ok ")
        (display name)
        (newline))
      (begin
        (set! *fail-count* (+ *fail-count* 1))
        (display "FAIL ")
        (display name)
        (display ": expected ")
        (write expected)
        (display ", got ")
        (write actual)
        (newline))))

(define (check-eqv name expected actual)
  (if (eqv? expected actual)
      (begin
        (set! *pass-count* (+ *pass-count* 1))
        (display "ok ")
        (display name)
        (newline))
      (begin
        (set! *fail-count* (+ *fail-count* 1))
        (display "FAIL ")
        (display name)
        (display ": expected ")
        (write expected)
        (display ", got ")
        (write actual)
        (newline))))

(define (check-eq name expected actual)
  (if (eq? expected actual)
      (begin
        (set! *pass-count* (+ *pass-count* 1))
        (display "ok ")
        (display name)
        (newline))
      (begin
        (set! *fail-count* (+ *fail-count* 1))
        (display "FAIL ")
        (display name)
        (display ": expected ")
        (write expected)
        (display ", got ")
        (write actual)
        (newline))))

(define (check-assert name bool)
  (if bool
      (begin
        (set! *pass-count* (+ *pass-count* 1))
        (display "ok ")
        (display name)
        (newline))
      (begin
        (set! *fail-count* (+ *fail-count* 1))
        (display "FAIL ")
        (display name)
        (newline))))

(define (check-finish)
  (display *pass-count*)
  (display " pass, ")
  (display *fail-count*)
  (display " fail")
  (newline)
  (if (> *fail-count* 0)
      (exit 1)
      (exit 0)))
