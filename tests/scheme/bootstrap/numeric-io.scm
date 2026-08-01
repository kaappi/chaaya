;; Numeric/IO hardening regression suite (Phase 7B).

;; Literal immutability failure paths are covered in C tests
;; (tests/c/test_primitives.c), where host-side assertions can
;; observe expected VM runtime failures.
(check-equal "mutable vector still updates"
             '#(1 9 3)
             (let ((v (vector 1 2 3)))
               (vector-set! v 1 9)
               v))

;; Port refill and incremental datum reads.
(define tmp-path "numeric-io.tmp")
(call-with-output-file
  tmp-path
  (lambda (out)
    (display (make-string 5000 #\space) out)
    (display "(alpha beta)" out)
    (newline out)
    (display "42" out)))

(call-with-input-file
  tmp-path
  (lambda (in)
    (check-equal "read skips buffered whitespace" '(alpha beta) (read in))
    (check-eqv "read second datum from same port" 42 (read in))
    (check-assert "read returns eof after data" (eof-object? (read in)))))
(delete-file tmp-path)

;; string-ref bounds + Unicode codepoint ordering for chars.
(define greek
  (list->string (list (integer->char 945) (integer->char 946)))) ; alpha beta
(check-eqv "string-length counts codepoints" 2 (string-length greek))
(check-eqv "string-ref first codepoint" 945 (char->integer (string-ref greek 0)))
(check-eqv "string-ref second codepoint" 946 (char->integer (string-ref greek 1)))
(check-assert "char<? compares Unicode codepoints"
              (char<? (string-ref greek 0) (string-ref greek 1)))
(check-eqv "string-set! replaces same-width codepoint"
           947
           (let ((s (list->string (list (integer->char 945) (integer->char 946)))))
             (string-set! s 0 (integer->char 947))
             (char->integer (string-ref s 0))))

;; eqv? exact/inexact hardening.
(check-assert "eqv? equal flonums" (eqv? 2.0 2.0))
(check-assert "eqv? mixed exactness false" (not (eqv? 2 2.0)))
(check-assert "eqv? distinguishes signed zero" (not (eqv? 0.0 -0.0)))
(check-assert "= treats signed zero as equal" (= 0.0 -0.0))
(check-assert "eqv? equal bignums" (eqv? (expt 2 100) (expt 2 100)))
(check-assert "eqv? equal reduced rationals" (eqv? 1/2 2/4))

(check-finish)
