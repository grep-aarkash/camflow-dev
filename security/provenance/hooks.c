/*
*
* /linux/security/provenance/hooks.c
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

#include <linux/provenance.h>
#include <linux/ifc.h>
#include <linux/slab.h>
#include <linux/lsm_hooks.h>
#include <linux/msg.h>
#include <net/sock.h>
#include <linux/binfmts.h>
#include <linux/random.h>
#include <linux/xattr.h>
#include <linux/camflow.h>

struct kmem_cache *provenance_cache=NULL;
struct kmem_cache *long_provenance_cache=NULL;

/*
 * initialise the security for the init task
 */
static void cred_init_provenance(void)
{
	struct cred *cred = (struct cred *) current->real_cred;
	prov_msg_t *prov = alloc_provenance(MSG_TASK, GFP_KERNEL);
	if (!prov)
		panic("Provenance:  Failed to initialize initial task.\n");
  set_node_id(prov, ASSIGN_NODE_ID);
  prov->task_info.uid=__kuid_val(cred->euid);
  prov->task_info.gid=__kgid_val(cred->egid);

	cred->provenance = prov;
}

/*
* @cred points to the credentials.
* @gfp indicates the atomicity of any memory allocations.
* Only allocate sufficient memory and attach to @cred such that
* cred_transfer() will not get ENOMEM.
*/
static int provenance_cred_alloc_blank(struct cred *cred, gfp_t gfp)
{
  prov_msg_t* prov  = alloc_provenance(MSG_TASK, gfp);

  if(!prov)
    return -ENOMEM;
  set_node_id(prov, ASSIGN_NODE_ID);

  prov->task_info.uid=__kuid_val(cred->euid);
  prov->task_info.gid=__kgid_val(cred->egid);

  cred->provenance = prov;
  return 0;
}

/*
* @cred points to the credentials.
* Deallocate and clear the cred->security field in a set of credentials.
*/
static void provenance_cred_free(struct cred *cred)
{
  free_provenance(cred->provenance);
  cred->provenance = NULL;
}

/*
* @new points to the new credentials.
* @old points to the original credentials.
* @gfp indicates the atomicity of any memory allocations.
* Prepare a new set of credentials by copying the data from the old set.
*/
static int provenance_cred_prepare(struct cred *new, const struct cred *old, gfp_t gfp)
{
  prov_msg_t* old_prov = old->provenance;
  prov_msg_t* prov = alloc_provenance(MSG_TASK, gfp);
#ifdef CONFIG_SECURITY_IFC
	struct ifc_struct *new_ifc;
#endif

  if(!prov){
    return -ENOMEM;
  }
  set_node_id(prov, ASSIGN_NODE_ID);
  prov->task_info.uid=__kuid_val(new->euid);
  prov->task_info.gid=__kgid_val(new->egid);

#ifdef CONFIG_SECURITY_IFC
	new_ifc = new->ifc;
	if(ifc_is_labelled(&new_ifc->context)){
		prov->node_info.node_kern.tracked=NODE_TRACKED;
		prov_record_ifc(prov, &new_ifc->context);
	}
#endif

  record_edge(ED_FORK, old_prov, prov, FLOW_ALLOWED);
  new->provenance = prov;
  return 0;
}

/*
* @new points to the new credentials.
* @old points to the original credentials.
* Transfer data from original creds to new creds
*/
static void provenance_cred_transfer(struct cred *new, const struct cred *old)
{
  const prov_msg_t *old_prov = old->provenance;
	prov_msg_t *prov = new->provenance;

  *prov=*old_prov;
}

/*
* Update the module's state after setting one or more of the user
* identity attributes of the current process.  The @flags parameter
* indicates which of the set*uid system calls invoked this hook.  If
* @new is the set of credentials that will be installed.  Modifications
* should be made to this rather than to @current->cred.
* @old is the set of credentials that are being replaces
* @flags contains one of the LSM_SETID_* values.
* Return 0 on success.
*/
static int provenance_task_fix_setuid(struct cred *new, const struct cred *old, int flags)
{
  prov_msg_t *old_prov = old->provenance;
	prov_msg_t *prov = new->provenance;

  record_edge(ED_CHANGE, old_prov, prov, FLOW_ALLOWED);
  return 0;
}

/*
* Allocate and attach a security structure to @inode->i_security.  The
* i_security field is initialized to NULL when the inode structure is
* allocated.
* @inode contains the inode structure.
* Return 0 if operation was successful.
*/
static int provenance_inode_alloc_security(struct inode *inode)
{
	prov_msg_t* cprov = current_provenance();
  prov_msg_t* iprov = alloc_provenance(MSG_INODE, GFP_KERNEL);
  prov_msg_t* sprov;
#ifdef CONFIG_SECURITY_IFC
	struct ifc_struct *ifc=NULL;
#endif

  if(unlikely(!iprov))
    return -ENOMEM;
  set_node_id(iprov, inode->i_ino);

  iprov->inode_info.uid=__kuid_val(inode->i_uid);
  iprov->inode_info.gid=__kgid_val(inode->i_gid);
  iprov->inode_info.mode=inode->i_mode;
  sprov = inode->i_sb->s_provenance;
  memcpy(iprov->inode_info.sb_uuid, sprov->sb_info.uuid, 16*sizeof(uint8_t));

	alloc_camflow(inode, GFP_KERNEL);
  inode_set_provenance(inode, (void**)&iprov);

#ifdef CONFIG_SECURITY_IFC
	ifc = inode_get_ifc(inode);
	if(ifc_is_labelled(&ifc->context)){
		iprov->node_info.node_kern.tracked=NODE_TRACKED;
		prov_record_ifc(iprov, &ifc->context);
	}
#endif

  record_edge(ED_CREATE, cprov, iprov, FLOW_ALLOWED); /* creating inode != creating the file */
  return 0;
}

/*
* @inode contains the inode structure.
* Deallocate the inode security structure and set @inode->i_security to
* NULL.
*/
static void provenance_inode_free_security(struct inode *inode)
{
  prov_msg_t* prov = inode_get_provenance(inode);
  if(!prov)
    free_provenance(prov);
	inode_set_provenance(inode, NULL);
	free_camflow(inode);
}

/*
* Check permission before accessing an inode.  This hook is called by the
* existing Linux permission function, so a security module can use it to
* provide additional checking for existing Linux permission checks.
* Notice that this hook is called when a file is opened (as well as many
* other operations), whereas the file_security_ops permission hook is
* called when the actual read/write operations are performed.
* @inode contains the inode structure to check.
* @mask contains the permission mask.
* Return 0 if permission is granted.
*/
static int provenance_inode_permission(struct inode *inode, int mask)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* iprov;

	if(unlikely(IS_PRIVATE(inode)))
		return 0;

	iprov = inode_get_provenance(inode);
  if(!iprov){ // alloc provenance if none there
    provenance_inode_alloc_security(inode);
		iprov = inode_get_provenance(inode);
  }

  mask &= (MAY_READ|MAY_WRITE|MAY_EXEC|MAY_APPEND);

  if((mask & (MAY_WRITE|MAY_APPEND)) != 0){
    record_edge(ED_WRITE, cprov, iprov, FLOW_ALLOWED);
  }
  if((mask & (MAY_READ)) != 0){
    record_edge(ED_READ, iprov, cprov, FLOW_ALLOWED);
  }
	if((mask & (MAY_EXEC)) != 0){
    record_edge(ED_EXEC, iprov, cprov, FLOW_ALLOWED);
  }
  return 0;
}

/*
* Check permission before creating a new hard link to a file.
* @old_dentry contains the dentry structure for an existing
* link to the file.
* @dir contains the inode structure of the parent directory
* of the new link.
* @new_dentry contains the dentry structure for the new link.
* Return 0 if permission is granted.
*/

static int provenance_inode_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry)
{
	/*
	prov_msg_t* cprov = current_provenance();
  prov_msg_t* dprov;
  prov_msg_t* iprov;
  long_prov_msg_t* link_prov;

  if(!inode_get_provenance(dir)){ // alloc provenance if none there
    provenance_inode_alloc_security(dir);
  }

  if(!inode_get_provenance(old_dentry->d_inode)){ // alloc provenance if none there
    provenance_inode_alloc_security(old_dentry->d_inode);
  }

  dprov = inode_get_provenance(dir); // directory
  iprov = inode_get_provenance(old_dentry->d_inode); // inode pointed by dentry

  // writing to the directory
  record_edge(ED_DATA, cprov, dprov, FLOW_ALLOWED);
  record_edge(ED_DATA, cprov, iprov, FLOW_ALLOWED);

  if(prov_enabled && (provenance_is_tracked(iprov) || provenance_is_tracked(dprov) || provenance_is_tracked(cprov))){
    link_prov = alloc_long_provenance(MSG_LINK, GFP_KERNEL);
    link_prov->link_info.length = new_dentry->d_name.len;
    memcpy(link_prov->link_info.name, new_dentry->d_name.name, new_dentry->d_name.len);
		copy_node_info(&link_prov->link_info.dir, &dprov->inode_info.node_info);
		copy_node_info(&link_prov->link_info.task, &cprov->task_info.node_info);
		copy_node_info(&link_prov->link_info.inode, &iprov->task_info.node_info);
    long_prov_write(link_prov);
    free_long_provenance(link_prov);
  }
	TODO link new file name node as alternative
	*/
  return 0;
}

static inline void provenance_record_file_name(struct file *file){
	struct inode *inode = file_inode(file);
	prov_msg_t *iprov = inode_get_provenance(inode);
	long_prov_msg_t *fname_prov;
	char *buffer;
	char *ptr;

	if(!provenance_is_name_recorded(iprov) && provenance_is_tracked(iprov)){
		buffer = (char*)kzalloc(PATH_MAX, GFP_KERNEL);
		fname_prov = alloc_long_provenance(MSG_FILE_NAME, GFP_KERNEL);
		ptr = dentry_path_raw(file->f_path.dentry, buffer, PATH_MAX);
		strlcpy(fname_prov->file_name_info.name, ptr, PATH_MAX);
		kfree(buffer);
		fname_prov->file_name_info.length=strlen(fname_prov->file_name_info.name);
		long_prov_write(fname_prov);
		long_record_edge(ED_NAMED, iprov, fname_prov, FLOW_ALLOWED);
		free_long_provenance(fname_prov);
		iprov->node_info.node_kern.name_recorded=NAME_RECORDED;
	}
}

/*
* Check file permissions before accessing an open file.  This hook is
* called by various operations that read or write files.  A security
* module can use this hook to perform additional checking on these
* operations, e.g.  to revalidate permissions on use to support privilege
* bracketing or policy changes.  Notice that this hook is used when the
* actual read/write operations are performed, whereas the
* inode_security_ops hook is called when a file is opened (as well as
* many other operations).
* Caveat:  Although this hook can be used to revalidate permissions for
* various system call operations that read or write files, it does not
* address the revalidation of permissions for memory-mapped files.
* Security modules must handle this separately if they need such
* revalidation.
* @file contains the file structure being accessed.
* @mask contains the requested permissions.
* Return 0 if permission is granted.
*/
static int provenance_file_permission(struct file *file, int mask)
{
  struct inode *inode = file_inode(file);
	provenance_record_file_name(file);
  provenance_inode_permission(inode, mask);
  return 0;
}

/*
* Save open-time permission checking state for later use upon
* file_permission, and recheck access if anything has changed
* since inode_permission.
*/
static int provenance_file_open(struct file *file, const struct cred *cred)
{
	prov_msg_t* cprov = current_provenance();
	struct inode *inode = file_inode(file);
	prov_msg_t* iprov;

	if(!inode_get_provenance(inode)){ // alloc provenance if none there
    provenance_inode_alloc_security(inode);
  }
	provenance_record_file_name(file);

	iprov = inode_get_provenance(inode);
	record_edge(ED_OPEN, iprov, cprov, FLOW_ALLOWED);
	return 0;
}

/*
* Check permissions for a mmap operation.  The @file may be NULL, e.g.
* if mapping anonymous memory.
* @file contains the file structure for file to map (may be NULL).
* @reqprot contains the protection requested by the application.
* @prot contains the protection that will be applied by the kernel.
* @flags contains the operational flags.
* Return 0 if permission is granted.
*/
static int provenance_mmap_file(struct file *file, unsigned long reqprot, unsigned long prot, unsigned long flags)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* iprov;
  struct inode *inode;

  if(file==NULL){ // what to do for NULL?
    return 0;
  }
	provenance_record_file_name(file);

  inode = file_inode(file);
  iprov = inode_get_provenance(inode);
  prot &= (PROT_EXEC|PROT_READ|PROT_WRITE);

  if((prot & (PROT_WRITE|PROT_EXEC)) != 0){
    record_edge(ED_MMAP, cprov, iprov, FLOW_ALLOWED);
  }
  if((prot & (PROT_READ|PROT_EXEC|PROT_WRITE)) != 0){
    // we assume write imply read
    record_edge(ED_MMAP, iprov, cprov, FLOW_ALLOWED);
  }
  return 0;
}

/*
* @file contains the file structure.
* @cmd contains the operation to perform.
* @arg contains the operational arguments.
* Check permission for an ioctl operation on @file.  Note that @arg
* sometimes represents a user space pointer; in other cases, it may be a
* simple integer value.  When @arg represents a user space pointer, it
* should never be used by the security module.
* Return 0 if permission is granted.
*/
static int provenance_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* iprov;
  struct inode *inode = file_inode(file);

  if(!inode_get_provenance(inode)){ // alloc provenance if none there
    provenance_inode_alloc_security(inode);
  }
	provenance_record_file_name(file);

  iprov = inode_get_provenance(inode);

	// both way exchange
  record_edge(ED_WRITE, cprov, iprov, FLOW_ALLOWED);
	prov_update_version(iprov);
  record_edge(ED_READ, iprov, cprov, FLOW_ALLOWED);
	prov_update_version(cprov);

  return 0;
}

/* msg */

/*
* Allocate and attach a security structure to the msg->security field.
* The security field is initialized to NULL when the structure is first
* created.
* @msg contains the message structure to be modified.
* Return 0 if operation was successful and permission is granted.
*/
static int provenance_msg_msg_alloc_security(struct msg_msg *msg)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* mprov;
#ifdef CONFIG_SECURITY_IFC
	struct ifc_struct* ifc= msg->ifc;
#endif
  /* alloc new prov struct with generated id */
  mprov = alloc_provenance(MSG_MSG, GFP_KERNEL);

  if(!mprov)
    return -ENOMEM;

  set_node_id(mprov, ASSIGN_NODE_ID);
  mprov->msg_msg_info.type=msg->m_type;
#ifdef CONFIG_SECURITY_IFC
	if(!ifc){
		if(ifc_is_labelled(&ifc->context)){
			mprov->msg_msg_info.node_kern.tracked=NODE_TRACKED;
			prov_record_ifc(mprov, &ifc->context);
		}
	}
#endif
  msg->provenance = mprov;
  record_edge(ED_CREATE, cprov, mprov, FLOW_ALLOWED);
  return 0;
}

/*
* Deallocate the security structure for this message.
* @msg contains the message structure to be modified.
*/
static void provenance_msg_msg_free_security(struct msg_msg *msg)
{
	prov_msg_t *prov = msg->provenance;
  msg->provenance=NULL;
  free_provenance(prov);
}

/*
* Check permission before a message, @msg, is enqueued on the message
* queue, @msq.
* @msq contains the message queue to send message to.
* @msg contains the message to be enqueued.
* @msqflg contains operational flags.
* Return 0 if permission is granted.
*/
static int provenance_msg_queue_msgsnd(struct msg_queue *msq, struct msg_msg *msg, int msqflg)
{
  prov_msg_t* cprov = current_provenance();
  prov_msg_t* mprov = msg->provenance;
  record_edge(ED_WRITE, cprov, mprov, FLOW_ALLOWED);
  return 0;
}

/*
* Check permission before a message, @msg, is removed from the message
* queue, @msq.  The @target task structure contains a pointer to the
* process that will be receiving the message (not equal to the current
* process when inline receives are being performed).
* @msq contains the message queue to retrieve message from.
* @msg contains the message destination.
* @target contains the task structure for recipient process.
* @type contains the type of message requested.
* @mode contains the operational flags.
* Return 0 if permission is granted.
*/
static int provenance_msg_queue_msgrcv(struct msg_queue *msq, struct msg_msg *msg,
				    struct task_struct *target,
				    long type, int mode)
{
  prov_msg_t* cprov = target->cred->provenance;
  prov_msg_t* mprov = msg->provenance;

  record_edge(ED_READ, mprov, cprov, FLOW_ALLOWED);
  return 0;
}

/*
* Allocate and attach a security structure to the shp->shm_perm.security
* field.  The security field is initialized to NULL when the structure is
* first created.
* @shp contains the shared memory structure to be modified.
* Return 0 if operation was successful and permission is granted.
*/
static int provenance_shm_alloc_security(struct shmid_kernel *shp)
{
	prov_msg_t* cprov = current_provenance();
  prov_msg_t* sprov = alloc_provenance(MSG_SHM, GFP_KERNEL);
#ifdef CONFIG_SECURITY_IFC
	struct ifc_struct* ifc= shp->shm_perm.ifc;
#endif

  if(!sprov)
    return -ENOMEM;

  set_node_id(sprov, ASSIGN_NODE_ID);
  sprov->shm_info.mode=shp->shm_perm.mode;

#ifdef CONFIG_SECURITY_IFC
	if(!ifc){
		if(ifc_is_labelled(&ifc->context)){
			sprov->shm_info.node_kern.tracked=NODE_TRACKED;
			prov_record_ifc(sprov, &ifc->context);
		}
	}
#endif

  shp->shm_perm.provenance=sprov;
  record_edge(ED_ATTACH, sprov, cprov, FLOW_ALLOWED);
  record_edge(ED_ATTACH, cprov, sprov, FLOW_ALLOWED);
	return 0;
}

/*
* Deallocate the security struct for this memory segment.
* @shp contains the shared memory structure to be modified.
*/
static void provenance_shm_free_security(struct shmid_kernel *shp)
{
  free_provenance(shp->shm_perm.provenance);
  shp->shm_perm.provenance=NULL;
}

/*
* Check permissions prior to allowing the shmat system call to attach the
* shared memory segment @shp to the data segment of the calling process.
* The attaching address is specified by @shmaddr.
* @shp contains the shared memory structure to be modified.
* @shmaddr contains the address to attach memory region to.
* @shmflg contains the operational flags.
* Return 0 if permission is granted.
*/
static int provenance_shm_shmat(struct shmid_kernel *shp,
			     char __user *shmaddr, int shmflg)
{
  prov_msg_t* cprov = current_provenance();
	prov_msg_t* sprov = shp->shm_perm.provenance;

  if(!sprov)
    return -ENOMEM;

  if(shmflg & SHM_RDONLY){
    record_edge(ED_ATTACH, sprov, cprov, FLOW_ALLOWED);
  }else{
    record_edge(ED_ATTACH, sprov, cprov, FLOW_ALLOWED);
    record_edge(ED_ATTACH, cprov, sprov, FLOW_ALLOWED);
  }
	return 0;
}

/*
* Allocate and attach a security structure to the sk->sk_security field,
* which is used to copy security attributes between local stream sockets.
*/
static int provenance_sk_alloc_security(struct sock *sk, int family, gfp_t priority)
{
  prov_msg_t* skprov = alloc_provenance(MSG_SOCK, priority);

  if(!skprov)
    return -ENOMEM;
  set_node_id(skprov, ASSIGN_NODE_ID);

  sk->sk_provenance=skprov;
  return 0;
}

/*
* Deallocate security structure.
*/
static void provenance_sk_free_security(struct sock *sk)
{
	free_provenance(sk->sk_provenance);
	sk->sk_provenance = NULL;
}

/*
* This hook allows a module to update or allocate a per-socket security
* structure. Note that the security field was not added directly to the
* socket structure, but rather, the socket security information is stored
* in the associated inode.  Typically, the inode alloc_security hook will
* allocate and and attach security information to
* sock->inode->i_security.  This hook may be used to update the
* sock->inode->i_security field with additional information that wasn't
* available when the inode was allocated.
* @sock contains the newly created socket structure.
* @family contains the requested protocol family.
* @type contains the requested communications type.
* @protocol contains the requested protocol.
* @kern set to 1 if a kernel socket.
*/
static int provenance_socket_post_create(struct socket *sock, int family,
				      int type, int protocol, int kern)
{
  prov_msg_t* cprov  = current_provenance();
  prov_msg_t* iprov  = inode_get_provenance(SOCK_INODE(sock));
  prov_msg_t* skprov = NULL;

  if(kern){
    return 0;
  }

  if(!sock->sk->sk_provenance){
		provenance_sk_alloc_security(sock->sk, family, GFP_KERNEL);
	}
  skprov = sock->sk->sk_provenance;
  skprov->sock_info.type = type;
  skprov->sock_info.family = family;
  skprov->sock_info.protocol = protocol;
  record_edge(ED_CREATE, cprov, skprov, FLOW_ALLOWED);
  record_edge(ED_ASSOCIATE, skprov, iprov, FLOW_ALLOWED);

  return 0;
}

static inline void provenance_record_address(struct socket *sock, struct sockaddr *address, int addrlen){
	prov_msg_t* skprov = sock->sk->sk_provenance;
	long_prov_msg_t* addr_info = NULL;

	if(!provenance_is_name_recorded(skprov) && provenance_is_tracked(skprov)){
	  addr_info = alloc_long_provenance(MSG_ADDR, GFP_KERNEL);
	  addr_info->address_info.length=addrlen;
	  memcpy(&(addr_info->address_info.addr), address, addrlen);
	  long_prov_write(addr_info);
		long_record_edge(ED_NAMED, skprov, addr_info, FLOW_ALLOWED);
	  free_long_provenance(addr_info);
		skprov->sock_info.node_kern.name_recorded=NAME_RECORDED;
	}
}

/*
* Check permission before socket protocol layer bind operation is
* performed and the socket @sock is bound to the address specified in the
* @address parameter.
* @sock contains the socket structure.
* @address contains the address to bind to.
* @addrlen contains the length of address.
* Return 0 if permission is granted.
*/
static int provenance_socket_bind(struct socket *sock, struct sockaddr *address, int addrlen)
{
  prov_msg_t* cprov  = current_provenance();
  prov_msg_t* skprov = sock->sk->sk_provenance;

  if(cprov->task_info.node_kern.opaque==NODE_OPAQUE)
    return 0;

  if(!skprov)
    return -ENOMEM;

	provenance_record_address(sock, address, addrlen);
	record_edge(ED_BIND, cprov, skprov, FLOW_ALLOWED);

  return 0;
}

/*
* Check permission before socket protocol layer connect operation
* attempts to connect socket @sock to a remote address, @address.
* @sock contains the socket structure.
* @address contains the address of remote endpoint.
* @addrlen contains the length of address.
* Return 0 if permission is granted.
*/
static int provenance_socket_connect(struct socket *sock, struct sockaddr *address, int addrlen)
{
  prov_msg_t* cprov  = current_provenance();
  prov_msg_t* skprov = sock->sk->sk_provenance;

  if(cprov->task_info.node_kern.opaque==NODE_OPAQUE)
    return 0;

  if(!skprov)
    return -ENOMEM;

	provenance_record_address(sock, address, addrlen);
	record_edge(ED_CONNECT, cprov, skprov, FLOW_ALLOWED);

  return 0;
}

/*
* Check permission before socket protocol layer listen operation.
* @sock contains the socket structure.
* @backlog contains the maximum length for the pending connection queue.
* Return 0 if permission is granted.
*/
static int provenance_socket_listen(struct socket *sock, int backlog)
{
  prov_msg_t* cprov  = current_provenance();
  prov_msg_t* skprov = sock->sk->sk_provenance;

  record_edge(ED_LISTEN, cprov, skprov, FLOW_ALLOWED);
  return 0;
}

/*
* Check permission before transmitting a message to another socket.
* @sock contains the socket structure.
* @msg contains the message to be transmitted.
* @size contains the size of message.
* Return 0 if permission is granted.
*/
static int provenance_socket_sendmsg(struct socket *sock, struct msghdr *msg,
				  int size)
{
	return provenance_inode_permission(SOCK_INODE(sock), MAY_WRITE);
}

/*
* Check permission before receiving a message from a socket.
* @sock contains the socket structure.
* @msg contains the message structure.
* @size contains the size of message structure.
* @flags contains the operational flags.
* Return 0 if permission is granted.
*/
static int provenance_socket_recvmsg(struct socket *sock, struct msghdr *msg,
				  int size, int flags)
{
	return provenance_inode_permission(SOCK_INODE(sock), MAY_READ);
}

/*
* Check permission before accepting a new connection.  Note that the new
* socket, @newsock, has been created and some information copied to it,
* but the accept operation has not actually been performed.
* @sock contains the listening socket structure.
* @newsock contains the newly created server socket for connection.
* Return 0 if permission is granted.
*/
static int provenance_socket_accept(struct socket *sock, struct socket *newsock)
{
  prov_msg_t* cprov  = current_provenance();
  prov_msg_t* skprov = inode_get_provenance(SOCK_INODE(sock));
  prov_msg_t* nskprov = inode_get_provenance(SOCK_INODE(newsock));
  record_edge(ED_CREATE, skprov, nskprov, FLOW_ALLOWED);
  record_edge(ED_ACCEPT, nskprov, cprov, FLOW_ALLOWED);
  return 0;
}

/*
* Check permissions before establishing a Unix domain stream connection
* between @sock and @other.
* @sock contains the sock structure.
* @other contains the peer sock structure.
* @newsk contains the new sock structure.
* Return 0 if permission is granted.
*/
static int provenance_unix_stream_connect(struct sock *sock,
					      struct sock *other,
					      struct sock *newsk)
{
  prov_msg_t* cprov  = current_provenance();
  prov_msg_t* skprov = sock->sk_provenance;
  prov_msg_t* nskprov = newsk->sk_provenance;
  prov_msg_t* okprov = other->sk_provenance;

  record_edge(ED_CONNECT, cprov, skprov, FLOW_ALLOWED);
  record_edge(ED_ASSOCIATE, skprov, nskprov, FLOW_ALLOWED);
  record_edge(ED_ASSOCIATE, skprov, okprov, FLOW_ALLOWED);
  return 0;
}

/*
* Check permissions before connecting or sending datagrams from @sock to
* @other.
* @sock contains the socket structure.
* @other contains the peer socket structure.
* Return 0 if permission is granted.
*/
static int provenance_unix_may_send(struct socket *sock,
					struct socket *other)
{
  prov_msg_t* skprov = sock->sk->sk_provenance;
  prov_msg_t* okprov = other->sk->sk_provenance;

  record_edge(ED_UNKNOWN, skprov, okprov, FLOW_ALLOWED);
  return 0;
}

/* outdated description */
/*
* Save security information in the bprm->security field, typically based
* on information about the bprm->file, for later use by the apply_creds
* hook.  This hook may also optionally check permissions (e.g. for
* transitions between security domains).
* This hook may be called multiple times during a single execve, e.g. for
* interpreters.  The hook can tell whether it has already been called by
* checking to see if @bprm->security is non-NULL.  If so, then the hook
* may decide either to retain the security information saved earlier or
* to replace it.
* @bprm contains the linux_binprm structure.
* Return 0 if the hook is successful and permission is granted.
*/
static int provenance_bprm_set_creds(struct linux_binprm *bprm){
  if(!bprm->cred->provenance){
    return provenance_cred_alloc_blank(bprm->cred, GFP_KERNEL);
  }
  return 0;
}

/*
* Tidy up after the installation of the new security attributes of a
* process being transformed by an execve operation.  The new credentials
* have, by this point, been set to @current->cred.  @bprm points to the
* linux_binprm structure.  This hook is a good place to perform state
* changes on the process such as clearing out non-inheritable signal
* state.  This is called immediately after commit_creds().
*/
 static void provenance_bprm_committing_creds(struct linux_binprm *bprm){
   prov_msg_t* cprov  = current_provenance();
   prov_msg_t* nprov = bprm->cred->provenance;
   struct inode *inode = file_inode(bprm->file);
   prov_msg_t* iprov = inode_get_provenance(inode);
   record_edge(ED_CREATE, cprov, nprov, FLOW_ALLOWED);
   record_edge(ED_CREATE, iprov, nprov, FLOW_ALLOWED);
 }

/*
* Allocate and attach a security structure to the sb->s_security field.
* The s_security field is initialized to NULL when the structure is
* allocated.
* @sb contains the super_block structure to be modified.
* Return 0 if operation was successful.
*/
static int provenance_sb_alloc_security(struct super_block *sb)
{
  prov_msg_t* sbprov  = alloc_provenance(MSG_SB, GFP_KERNEL);
  if(!sbprov)
    return -ENOMEM;
  sb->s_provenance = sbprov;
  return 0;
}

/*
* Deallocate and clear the sb->s_security field.
* @sb contains the super_block structure to be modified.
*/
static void provenance_sb_free_security(struct super_block *sb)
{
  free_provenance(sb->s_provenance);
  sb->s_provenance=NULL;
}

static int provenance_sb_kern_mount(struct super_block *sb, int flags, void *data)
{
  int i;
  uint8_t c=0;
  prov_msg_t* sbprov = sb->s_provenance;
  for(i=0; i<16; i++){
    sbprov->sb_info.uuid[i]=sb->s_uuid[i];
    c|=sb->s_uuid[i];
  }
  if(c==0){ // no uuid defined, generate random one
    get_random_bytes(sbprov->sb_info.uuid, 16*sizeof(uint8_t));
  }
  return 0;
}

static struct security_hook_list provenance_hooks[] = {
  LSM_HOOK_INIT(cred_alloc_blank, provenance_cred_alloc_blank),
  LSM_HOOK_INIT(cred_free, provenance_cred_free),
  LSM_HOOK_INIT(cred_prepare, provenance_cred_prepare),
  LSM_HOOK_INIT(cred_transfer, provenance_cred_transfer),
  LSM_HOOK_INIT(task_fix_setuid, provenance_task_fix_setuid),
  LSM_HOOK_INIT(inode_alloc_security, provenance_inode_alloc_security),
  LSM_HOOK_INIT(inode_free_security, provenance_inode_free_security),
  LSM_HOOK_INIT(inode_permission, provenance_inode_permission),
  LSM_HOOK_INIT(file_permission, provenance_file_permission),
  LSM_HOOK_INIT(mmap_file, provenance_mmap_file),
  LSM_HOOK_INIT(file_ioctl, provenance_file_ioctl),
  LSM_HOOK_INIT(inode_link, provenance_inode_link),
	LSM_HOOK_INIT(msg_msg_alloc_security, provenance_msg_msg_alloc_security),
	LSM_HOOK_INIT(msg_msg_free_security, provenance_msg_msg_free_security),
  LSM_HOOK_INIT(msg_queue_msgsnd, provenance_msg_queue_msgsnd),
  LSM_HOOK_INIT(msg_queue_msgrcv, provenance_msg_queue_msgrcv),
  LSM_HOOK_INIT(shm_alloc_security, provenance_shm_alloc_security),
  LSM_HOOK_INIT(shm_free_security, provenance_shm_free_security),
  LSM_HOOK_INIT(shm_shmat, provenance_shm_shmat),
  LSM_HOOK_INIT(sk_alloc_security, provenance_sk_alloc_security),
  LSM_HOOK_INIT(sk_free_security, provenance_sk_free_security),
  LSM_HOOK_INIT(socket_post_create, provenance_socket_post_create),
  LSM_HOOK_INIT(socket_bind, provenance_socket_bind),
  LSM_HOOK_INIT(socket_connect, provenance_socket_connect),
  LSM_HOOK_INIT(socket_listen, provenance_socket_listen),
  LSM_HOOK_INIT(socket_sendmsg, provenance_socket_sendmsg),
  LSM_HOOK_INIT(socket_recvmsg, provenance_socket_recvmsg),
  LSM_HOOK_INIT(socket_accept, provenance_socket_accept),
  LSM_HOOK_INIT(unix_stream_connect, provenance_unix_stream_connect),
  LSM_HOOK_INIT(unix_may_send, provenance_unix_may_send),
  LSM_HOOK_INIT(bprm_set_creds, provenance_bprm_set_creds),
  LSM_HOOK_INIT(bprm_committing_creds, provenance_bprm_committing_creds),
  LSM_HOOK_INIT(sb_alloc_security, provenance_sb_alloc_security),
  LSM_HOOK_INIT(sb_free_security, provenance_sb_free_security),
  LSM_HOOK_INIT(sb_kern_mount, provenance_sb_kern_mount),
	LSM_HOOK_INIT(file_open, provenance_file_open)
};
#ifndef CONFIG_SECURITY_IFC
struct kmem_cache *camflow_cache=NULL;
#endif

uint32_t prov_machine_id=1; /* TODO get a proper id somehow */
uint32_t prov_boot_id=0;
void __init provenance_add_hooks(void){
	printk(KERN_INFO "Provenance Camflow %s\n", CAMFLOW_VERSION_STR);
	get_random_bytes(&prov_boot_id, sizeof(uint32_t));
  provenance_cache = kmem_cache_create("provenance_struct",
					    sizeof(prov_msg_t),
					    0, SLAB_PANIC, NULL);
  long_provenance_cache = kmem_cache_create("long_provenance_struct",
					    sizeof(long_prov_msg_t),
					    0, SLAB_PANIC, NULL);
#ifndef CONFIG_SECURITY_IFC
	camflow_cache = kmem_cache_create("camflow_i_ptr",
							sizeof(struct camflow_i_ptr),
							0, SLAB_PANIC, NULL);
#endif
  cred_init_provenance();
  /* register the provenance security hooks */
  security_add_hooks(provenance_hooks, ARRAY_SIZE(provenance_hooks));
	printk(KERN_INFO "Provenance hooks ready.\n");
}
