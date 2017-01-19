#lang racket/base
(require "syntax.rkt"
         "to-list.rkt"
         "scope.rkt"
         "taint-dispatch.rkt"
         (rename-in "taint.rkt"
                    [syntax-tainted? raw:syntax-tainted?]
                    [syntax-arm raw:syntax-arm]
                    [syntax-disarm raw:syntax-disarm]
                    [syntax-rearm raw:syntax-rearm]
                    [syntax-taint raw:syntax-taint])
         (only-in "../expand/syntax-local.rkt" syntax-local-phase-level)
         "../namespace/core.rkt"
         "../namespace/inspector.rkt"
         "../common/contract.rkt")

;; Provides public versions of taint-related syntax functions

(provide syntax-tainted?
         syntax-arm
         syntax-disarm
         syntax-rearm
         syntax-taint)

(define (syntax-tainted? s)
  (check 'syntax-tainted? syntax? s)
  (raw:syntax-tainted? s))

(define (syntax-arm s [maybe-insp #f] [use-mode? #f])
  (check 'syntax-arm syntax? s)
  (unless (or (not maybe-insp)
              (inspector? maybe-insp))
    (raise-argument-error 'syntax-arm "(or/c inspector? #f)" maybe-insp))
  (define insp (inspector-for-taint maybe-insp))
  (cond
   [use-mode?
    (taint-dispatch
     s
     (lambda (s) (raw:syntax-arm s insp))
     (syntax-local-phase-level))]
   [else
    (raw:syntax-arm s insp)]))

(define (syntax-disarm s maybe-insp)
  (check 'syntax-disarm syntax? s)
  (unless (or (not maybe-insp)
              (inspector? maybe-insp))
    (raise-argument-error 'syntax-disarm "(or/c inspector? #f)" maybe-insp))
  (define insp (inspector-for-taint maybe-insp))
  (raw:syntax-disarm s insp))
  
(define (syntax-rearm s from-s [use-mode? #f])
  (check 'syntax-disarm syntax? s)
  (check 'syntax-disarm syntax? from-s)
  (cond
   [use-mode? (taint-dispatch
               s
               (lambda (s) (raw:syntax-rearm s from-s))
               (syntax-local-phase-level))]
   [else
    (raw:syntax-rearm s from-s)]))

(define (syntax-taint s)
  (check 'syntax-taint syntax? s)
  (raw:syntax-taint s))

;; ----------------------------------------

(define (inspector-for-taint maybe-insp)
  (or maybe-insp
      (current-module-code-inspector)
      (current-code-inspector)))

;; ----------------------------------------