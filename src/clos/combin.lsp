;;;;  -*- Mode: Lisp; Syntax: Common-Lisp; Package: CLOS -*-
;;;;
;;;;  Copyright (c) 1992, Giuseppe Attardi.
;;;;  Copyright (c) 2010-2015, Jean-Claude Beaudoin.
;;;;
;;;;    ECoLisp is free software; you can redistribute it and/or
;;;;    modify it under the terms of the GNU Lesser General Public
;;;;    License as published by the Free Software Foundation; either
;;;;    version 3 of the License, or (at your option) any later version.
;;;;
;;;;    See file '../../Copyright' for full details.

(in-package "CLOS")

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;; COMPILING EFFECTIVE METHODS
;;;
;;; The following functions take care of transforming the forms
;;; produced by the method combinations into effective methods. In MKCL
;;; effective methods are nothing but directly callable functions.
;;; Ideally, this compilation should just produce new compiled
;;; functions. However, we do not want to cons a lot of functions, and
;;; therefore we use closures.
;;;
;;; Formerly we used to keep a list of precompiled effective methods
;;; and made a structural comparison between the current method and
;;; the precompiled ones, so as to save memory. This only causes
;;; improvements in declarative combinations. For standard combinations
;;; it should be enough with a couple of different closures and hence
;;; the structural comparison is a loss of time.
;;;
;;; This is the core routine. It produces effective methods (i.e.
;;; functions) out of the forms generated by the method combinators.
;;; We consider the following cases:
;;;  1) Ordinary methods. The function of the method is extracted.
;;;  2) Functions. They map to themselves. This only happens
;;;     when these functions have been generated by previous calls
;;;     to EFFECTIVE-METHOD-FUNCTION.
;;;  3) (CALL-METHOD method rest-methods) A closure is
;;;	generated that invokes the current method while informing
;;;	it about the remaining methods.
;;;  4) (MAKE-METHOD form) A function is created that takes the
;;;	list of arguments of the generic function and evaluates
;;;	the forms in a null environment. This is the only form
;;;	that may lead to consing of new bytecode objects. Nested
;;;	CALL-METHOD are handled via the global macro CALL-METHOD.
;;;  5) Ordinary forms are turned into lambda forms, much like
;;;	what happens with the content of MAKE-METHOD.
;;;
(defun effective-method-function (form &optional top-level)
  (cond ((functionp form)
	 form)
	((method-p form)
	 (wrapped-method-function (method-function form)))
	((atom form)
	 (error "Malformed effective method form:~%~A" form))
	((eq (first form) 'MAKE-METHOD)
	 (coerce `(lambda (.combined-method-args. *next-methods*)
		    (declare (special .combined-method-args. *next-methods*))
		    ,(second form))
		 'function))
	((eq (first form) 'CALL-METHOD)
	 (combine-method-functions
	  (effective-method-function (second form))
	  (mapcar #'effective-method-function (third form))))
	(top-level
	 (coerce `(lambda (.combined-method-args. no-next-methods)
		    (declare 
		     (special .combined-method-args.) ;; JCB
		     (ignorable no-next-methods))
		    ,form)
		 'function))
	(t
	 (error "Malformed effective method form:~%~A" form))))

(defun wrapped-method-function (method-function)
  #'(lambda (args next-methods)
      (let ((.combined-method-args. args)
	    (*next-methods* next-methods))
	(declare (special .combined-method-args. *next-methods*))
	(apply method-function args))))

;;;
;;; This function is a combinator of effective methods. It creates a
;;; closure that invokes the first method while passing the information
;;; of the remaining methods. The resulting closure (or effective method)
;;; is the equivalent of (CALL-METHOD method rest-methods)
;;;
(defun combine-method-functions (method rest-methods)
  #'(lambda (args no-next-methods)
      (declare (ignorable no-next-methods))
      (let ((.combined-method-args. args))
        (declare (special .combined-method-args.)) ;; JCB
        (funcall method args rest-methods)))
  )

(defmacro call-method (method &optional rest-methods)
  `(funcall ,(effective-method-function method)
	    (locally (declare (special .combined-method-args.))
		     .combined-method-args.)
	    ',(and rest-methods (mapcar #'effective-method-function rest-methods))))

(defun error-qualifier (m qualifier)
  (error "Standard method combination allows only one qualifier ~
          per method, either :BEFORE, :AFTER, or :AROUND; while ~
          a method with ~S was found."
	 m qualifier))

(defun standard-main-effective-method (before primary after)
  #'(lambda (.combined-method-args. no-next-method)
      (declare 
       (special .combined-method-args.) ;; JCB
       (ignorable no-next-method))
      (dolist (i before)
	(funcall i .combined-method-args. nil))
      (if after
	  (multiple-value-prog1
	   (funcall (first primary) .combined-method-args. (rest primary))
	   (dolist (i after)
	     (funcall i .combined-method-args. nil)))
	(funcall (first primary) .combined-method-args. (rest primary)))))

(defun standard-compute-effective-method (gf methods)
  (let* ((before ())
	 (primary ())
	 (after ())
	 (around ()))
    (dolist (m methods)
      (let* ((qualifiers (method-qualifiers m))
	     (f (wrapped-method-function (method-function m))))
	(cond ((null qualifiers) (push f primary))
	      ((rest qualifiers) (error-qualifier m qualifiers))
	      ((eq (setq qualifiers (first qualifiers)) :BEFORE)
	       (push f before))
	      ((eq qualifiers :AFTER) (push f after))
	      ((eq qualifiers :AROUND) (push f around))
	      (t (error-qualifier m qualifiers)))))
    ;; When there are no primary methods, an error is to be signaled,
    ;; and we need not care about :AROUND, :AFTER or :BEFORE methods.
    (when (null primary)
      (return-from standard-compute-effective-method
	#'(lambda (args next-methods)
	    (declare (ignore next-methods))
	    (apply #'no-applicable-primary-method gf args)
	    )))
    ;; PRIMARY, BEFORE and AROUND are reversed because they have to
    ;; be on most-specific-first order (ANSI 7.6.6.2), while AFTER
    ;; may remain as it is because it is least-specific-order.
    (setf primary (nreverse primary)
	  before (nreverse before))
    (if around
	(let ((main (if (or before after)
			(list
			 (standard-main-effective-method before primary after))
			primary)))
	  (setf around (nreverse around))
	  (combine-method-functions (first around)
				    (nconc (rest around) main)))
	(if (or before after)
	    (standard-main-effective-method before primary after)
	    (combine-method-functions (first primary) (rest primary))))
    ))

;; ----------------------------------------------------------------------
;; DEFINE-METHOD-COMBINATION
;;
;; METHOD-COMBINATION objects are just a list
;;	(name arg*)
;; where NAME is the name of the method combination type defined with
;; DEFINE-METHOD-COMBINATION, and ARG* is zero or more arguments.
;;
;; For each method combination type there is an associated function,
;; and the list of all known method combination types is kept in
;; *METHOD-COMBINATIONS* in the form of property list:
;;	(mc-type-name1 function1 mc-type-name2 function2 ....)
;;
;; FUNCTIONn is the function associated to a method combination. It
;; is of type (FUNCTION (generic-function method-list) FUNCTION),
;; and it outputs an anonymous function which is the effective method.
;;

(defvar *method-combinations* '())

(defun install-method-combination (name function)
  (with-metadata-lock
   (setf (getf *method-combinations* name) function))
  name)

(defun define-simple-method-combination (name &key documentation
					 identity-with-one-argument
					 (operator name))
  (declare (ignore documentation))
  `(define-method-combination
     ,name (&optional (order :MOST-SPECIFIC-FIRST))
     ((around (:AROUND))
      (principal (,name) :REQUIRED t))
     (let ((main-effective-method
	    `(,',operator ,@(mapcar #'(lambda (x) `(CALL-METHOD ,x NIL))
				    (if (eql order :MOST-SPECIFIC-LAST)
					(reverse principal)
					principal)))))
       (cond (around
	      `(call-method ,(first around)
		(,@(rest around) (MAKE-METHOD ,main-effective-method))))
	     (,(if identity-with-one-argument
		   '(rest principal)
		   t)
	      main-effective-method)
	     (t (second main-effective-method))))))

(defun define-complex-method-combination (form)
  (flet ((syntax-error ()
	   (error "~S is not a valid DEFINE-METHOD-COMBINATION form" form)))
    (destructuring-bind (name lambda-list method-groups &rest body &aux
			 (group-names '())
			 (group-checks '())
			 (group-after '())
			 (generic-function '.generic-function.)
			 (method-arguments '()))
	form
      (unless (symbolp name) (syntax-error))
      (let ((x (first body)))
	(when (and (consp x) (eql (first x) :ARGUMENTS))
	  (error "Option :ARGUMENTS is not supported in DEFINE-METHOD-COMBINATION.")))
      (let ((x (first body)))
	(when (and (consp x) (eql (first x) :GENERIC-FUNCTION))
	  (setf body (rest body))
	  (unless (symbolp (setf generic-function (second x)))
	    (syntax-error))))
      (dolist (group method-groups)
	(destructuring-bind (name predicate &key description
				  (order :most-specific-first) (required nil))
	    group
	  (declare (ignore description))
	  (if (symbolp name)
	      (push name group-names)
	      (syntax-error))
	  (let ((condition
		(cond ((eql predicate '*) 'T)
		      ((and predicate (symbolp predicate))
                       `(,predicate .METHOD-QUALIFIERS.))
		      ((and (listp predicate)
			    (let* ((q (last predicate 0))
				   (p (copy-list (butlast predicate 0))))
			      (when (every #'symbolp p)
				(if (eql q '*)
				    `(every #'equal ',p .METHOD-QUALIFIERS.)
				    `(equal ',p .METHOD-QUALIFIERS.))))))
		      (t (syntax-error)))))
	    (push `(,condition (push .METHOD. ,name)) group-checks))
	  (when required
	    (push `(unless ,name
		    (invalid-method-error "Method combination: ~S. No methods ~
					   in required group ~S." ,name))
		  group-after))
	  (case order
	    (:most-specific-first
	     (push `(setf ,name (nreverse ,name)) group-after))
	    (:most-specific-last)
	    (otherwise (syntax-error)))))
      `(si::define-when (:load-toplevel :execute)
	 (install-method-combination ',name
	   (si::lambda-block ,name (,generic-function .methods-list. ,@lambda-list)
	       (declare (ignorable ,generic-function))
	     (let (,@group-names)
	       (dolist (.method. .methods-list.)
		 (let ((.method-qualifiers. (method-qualifiers .method.)))
		   (cond ,@(nreverse group-checks)
			 (t (invalid-method-error .method.
			      "Method qualifiers ~S are not allowed in the method~
			       combination ~S." .method-qualifiers. ',name)))))
	       ,@group-after
	       (effective-method-function (progn ,@body) t)))))
      )))

(defmacro define-method-combination (name &body body)
  (if (and body (listp (first body)))
      (define-complex-method-combination (list* name body))
    (apply #'define-simple-method-combination name body)))

(defun method-combination-error (format-control &rest args)
  ;; FIXME! We should emit a more detailed error!
  (error "Method-combination error:~%~S" (apply #'format nil format-control args)))

(defun invalid-method-error (method format-control &rest args)
  (error "Invalid method error for ~A~%~S" method (apply #'format nil format-control args)))

;;; ----------------------------------------------------------------------
;;; COMPUTE-EFFECTIVE-METHOD
;;;

(defun compute-effective-method (gf method-combination applicable-methods)
  (let* ((method-combination-name (car method-combination))
	 (method-combination-args (cdr method-combination)))
    (if (eq method-combination-name 'STANDARD)
	(standard-compute-effective-method gf applicable-methods)
	(apply (or (getf *method-combinations* method-combination-name)
		   (error "~S is not a valid method combination object" method-combination))
	       gf applicable-methods
	       method-combination-args))))

(defun compute-effective-method-for-cache (gf args)
  (with-metadata-lock
   (let ((applicable-methods (compute-applicable-methods gf args)))
     (if applicable-methods
         (compute-effective-method gf (generic-function-method-combination gf) applicable-methods)
       #'(lambda (args next-methods)
	    (declare (ignore next-methods))
           (apply #'no-applicable-method gf args))
       ))))

;;
;; These method combinations are bytecompiled, for simplicity.
;;
(eval '(progn
	(define-method-combination progn :identity-with-one-argument t)
	(define-method-combination and :identity-with-one-argument t)
	(define-method-combination max :identity-with-one-argument t)
	(define-method-combination + :identity-with-one-argument t)
	(define-method-combination nconc :identity-with-one-argument t)
	(define-method-combination append :identity-with-one-argument nil)
	(define-method-combination list :identity-with-one-argument nil)
	(define-method-combination min :identity-with-one-argument t)
	(define-method-combination or :identity-with-one-argument t)))

