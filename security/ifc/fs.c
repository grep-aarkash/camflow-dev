/*
*
* /linux/security/ifc/fs.c
*
* Author: Thomas Pasquier <tfjmp2@cam.ac.uk>
*
* Copyright (C) 2015 University of Cambridge
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2, as
* published by the Free Software Foundation.
*
*/

#include <linux/security.h>
#include <linux/fs.h>
#include <linux/ifc.h>

static inline struct ifc_context* context_from_pid(pid_t pid){
  struct task_struct *dest = find_task_by_vpid(pid);
  if(!dest)
    return NULL;
  return __task_cred(dest)->ifc;
}

static ssize_t ifc_write_self(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)

{
  struct ifc_context *cifc = current_ifc();
  struct ifc_tag_msg *msg;
  int rv=-EINVAL;

  if(count < sizeof(struct ifc_tag_msg)){
    return -ENOMEM;
  }

  msg = (struct ifc_tag_msg*)buf;

  if(!ifc_tag_valid(msg->tag)){
    return -EINVAL;
  }

  if(msg->op==IFC_ADD_TAG){
    switch(msg->tag_type){
      case IFC_SECRECY:
        rv=ifc_add_tag(cifc, IFC_SECRECY, msg->tag);
        break;
      case IFC_INTEGRITY:
        rv=ifc_add_tag(cifc, IFC_INTEGRITY, msg->tag);
        break;
      case IFC_SECRECY_P:
        rv=ifc_add_privilege(cifc, IFC_SECRECY_P, msg->tag);
        break;
      case IFC_INTEGRITY_P:
        rv=ifc_add_privilege(cifc, IFC_INTEGRITY_P, msg->tag);
        break;
      case IFC_SECRECY_N:
        rv=ifc_add_privilege(cifc, IFC_SECRECY_N, msg->tag);
        break;
      case IFC_INTEGRITY_N:
        rv=ifc_add_privilege(cifc, IFC_INTEGRITY_N, msg->tag);
        break;
    }
  }else{
    switch(msg->tag_type){
      case IFC_SECRECY:
        rv=ifc_remove_tag(cifc, IFC_SECRECY, msg->tag);
        break;
      case IFC_INTEGRITY:
        rv=ifc_remove_tag(cifc, IFC_INTEGRITY, msg->tag);
        break;
      case IFC_SECRECY_P:
        rv=ifc_remove_privilege(cifc, IFC_SECRECY_P, msg->tag);
        break;
      case IFC_INTEGRITY_P:
        rv=ifc_remove_privilege(cifc, IFC_INTEGRITY_P, msg->tag);
        break;
      case IFC_SECRECY_N:
        rv=ifc_remove_privilege(cifc, IFC_SECRECY_N, msg->tag);
        break;
      case IFC_INTEGRITY_N:
        rv=ifc_remove_privilege(cifc, IFC_INTEGRITY_N, msg->tag);
        break;
    }
  }
  if(!rv){
    return sizeof(struct ifc_tag_msg);
  }
  return rv; // return error
}

static ssize_t ifc_read_self(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
  struct ifc_context *cifc = current_ifc();

	if(count < sizeof(struct ifc_context)){
    return -ENOMEM;
  }
  if(copy_to_user(buf, cifc, sizeof(struct ifc_context))){
    return -EAGAIN;
  }
  return sizeof(struct ifc_context);
}

static const struct file_operations ifc_self_ops = {
	.write		= ifc_write_self,
  .read     = ifc_read_self,
	.llseek		= generic_file_llseek,
};

static ssize_t ifc_write_tag(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)

{
	return -EPERM; // does nothing for now
}

static ssize_t ifc_read_tag(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct ifc_context *cifc = current_ifc();
	tag_t tag;
	int rv=0;

	if(count<sizeof(tag_t))
		return -ENOMEM;

	tag = ifc_create_tag();

	rv |= ifc_add_privilege(cifc, IFC_SECRECY_P, tag);
	rv |= ifc_add_privilege(cifc, IFC_INTEGRITY_P, tag);
	rv |= ifc_add_privilege(cifc, IFC_SECRECY_N, tag);
	rv |= ifc_add_privilege(cifc, IFC_INTEGRITY_N, tag);

	if(rv<0){
		return rv;
	}

	if(copy_to_user(buf, &tag, sizeof(tag_t))){
    return -EAGAIN;
  }

	return sizeof(tag_t);
}

static const struct file_operations ifc_tag_ops = {
	.write		= ifc_write_tag,
  .read     = ifc_read_tag,
	.llseek		= generic_file_llseek,
};

static ssize_t ifc_write_process(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)

{
	struct ifc_context *cifc = current_ifc();
	struct ifc_context *oifc = NULL;
  struct ifc_tag_msg *msg;
  int rv=-EINVAL;

  if(count < sizeof(struct ifc_tag_msg)){
    return -ENOMEM;
  }

  msg = (struct ifc_tag_msg*)buf;

  if(!ifc_tag_valid(msg->tag)){
    return -EINVAL;
  }

	oifc = context_from_pid(msg->pid);
	if(!oifc){ // did not find anything
		return -EINVAL;
	}

  if(msg->op==IFC_ADD_TAG){
    switch(msg->tag_type){
      case IFC_SECRECY_P:
				if(!ifc_contains_value(&cifc->secrecy_p, msg->tag))
					return -EPERM;
        rv=ifc_add_privilege(oifc, IFC_SECRECY_P, msg->tag);
        break;
      case IFC_INTEGRITY_P:
				if(!ifc_contains_value(&cifc->integrity_p, msg->tag))
					return -EPERM;
        rv=ifc_add_privilege(oifc, IFC_INTEGRITY_P, msg->tag);
        break;
      case IFC_SECRECY_N:
				if(!ifc_contains_value(&cifc->secrecy_n, msg->tag))
					return -EPERM;
        rv=ifc_add_privilege(oifc, IFC_SECRECY_N, msg->tag);
        break;
      case IFC_INTEGRITY_N:
				if(!ifc_contains_value(&cifc->integrity_n, msg->tag))
					return -EPERM;
        rv=ifc_add_privilege(oifc, IFC_INTEGRITY_N, msg->tag);
        break;
    }
  }
  if(!rv){
    return sizeof(struct ifc_tag_msg);
  }
  return rv; // return error
}

static ssize_t ifc_read_process(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct ifc_context *oifc = NULL;
  struct ifc_context_msg *msg;

  if(count < sizeof(struct ifc_context_msg)){
    return -ENOMEM;
  }

  msg = (struct ifc_context_msg*)buf;

  oifc = context_from_pid(msg->pid);
	if(!oifc){ // did not find anything
		return -EINVAL;
	}

  if(copy_to_user(&msg->context, oifc, sizeof(struct ifc_context))){
    return -EAGAIN;
  }
  return sizeof(struct ifc_context_msg);
}

static const struct file_operations ifc_process_ops = {
	.write		= ifc_write_process,
  .read     = ifc_read_process,
	.llseek		= generic_file_llseek,
};

static int __init init_ifc_fs(void)
{
   struct dentry *ifc_dir;

   ifc_dir = securityfs_create_dir("ifc", NULL);

   securityfs_create_file("self", 0666, ifc_dir, NULL, &ifc_self_ops);
	 securityfs_create_file("tag", 0644, ifc_dir, NULL, &ifc_tag_ops);
	 securityfs_create_file("process", 0666, ifc_dir, NULL, &ifc_process_ops);
   return 0;
}

__initcall(init_ifc_fs);
