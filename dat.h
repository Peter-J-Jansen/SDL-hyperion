/* DAT.H        (C) Copyright Roger Bowler, 1999-2012                */
/*              (C) and others 2013-2021                             */
/*              ESA/390 Dynamic Address Translation                  */
/*                                                                   */
/*   Released under "The Q Public License Version 1"                 */
/*   (http://www.hercules-390.org/herclic.html) as modifications to  */
/*   Hercules.                                                       */

/* Interpretive Execution - (C) Copyright Jan Jaeger, 1999-2012      */
/* z/Architecture support - (C) Copyright Jan Jaeger, 1999-2012      */

/*-------------------------------------------------------------------*/
/*   ARCH_DEP section: compiled multiple times, once for each arch.  */
/*   that use this header file.                                      */
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/*                           maddr_l                                 */
/*-------------------------------------------------------------------*/
/* For compatibility, it is usually invoked using the MADDRL macro   */
/* in feature.h                                                      */
/*-------------------------------------------------------------------*/
/* Convert logical address to absolute address. This is the DAT      */
/* logic that does an accelerated TLB lookup to return the prev-     */
/* iously determined value from an earlier translation for this      */
/* logical address.  It performs a series of checks to ensure the    */
/* values that were used in the previous translation (the results    */
/* of which are in the corresponding TLB entry) haven't changed      */
/* for the current address being translated.  If any of the cond-    */
/* itions have changed (i.e. if any of the comparisons fail) then    */
/* the TLB cannot be used (TLB miss) and "logical_to_main_l" is      */
/* called to perform a full address translation. Otherwise if all    */
/* of the conditions are still true (nothing has changed from the    */
/* the last time we translated this address), then the previously    */
/* translated address from the TLB is returned instead (TLB hit).    */
/*                                                                   */
/* PLEASE NOTE that the address that is retrieved from the TLB is    */
/* an absolute address from the Hercules guest's point of view but   */
/* the address RETURNED TO THE CALLER is a Hercules host address     */
/* pointing to MAINSTOR that Hercules can then directly use.         */
/*                                                                   */
/* Input:                                                            */
/*      addr    Logical address to be translated                     */
/*      len     Length of data access for PER SA purpose             */
/*      arn     Access register number or the special value:         */
/*                 USE_INST_SPACE                                    */
/*                 USE_REAL_ADDR                                     */
/*                 USE_PRIMARY_SPACE                                 */
/*                 USE_SECONDARY_SPACE                               */
/*                 USE_HOME_SPACE                                    */
/*                 USE_ARMODE + access register number               */
/*              An access register number ORed with the special      */
/*              value USE_ARMODE forces this routine to use AR-mode  */
/*              address translation regardless of the PSW address-   */
/*              space control setting.                               */
/*      regs    Pointer to the CPU register context                  */
/*      acctype Type of access requested: READ, WRITE, INSTFETCH,    */
/*              LRA, IVSK, TPROT, STACK, PTE, LPTEA                  */
/*      akey    Bits 0-3=access key, 4-7=zeroes                      */
/*                                                                   */
/* Returns:                                                          */
/*      Directly usable guest absolute storage MAINADDR address.     */
/*                                                                   */
/*-------------------------------------------------------------------*/
static inline  BYTE* ARCH_DEP( maddr_l )
    ( VADR addr, size_t len, const int arn, REGS* regs, const int acctype, const BYTE akey )
{
    /* Note: ALL of the below conditions must be true for a TLB hit
       to occur.  If ANY of them are false, then it's a TLB miss,
       requiring us to then perform a full DAT address translation.

       Note too that on the grand scheme of things the order/sequence
       of the below tests (if statements) is completely unimportant
       since ALL conditions must be checked anyway in order for a hit
       to occur, and it doesn't matter that a miss tests a few extra
       conditions since it's going to do a full translation anyway!
       (which is many, many instructions)
    */

    int  aea_crn  = (arn >= USE_ARMODE) ? 0 : regs->AEA_AR( arn );
    U16  tlbix    = TLBIX( addr );
    BYTE *maddr   = NULL;

    /* Non-zero AEA Control Register number? */
    if (aea_crn)
    {
        /* Same Addess Space Designator as before? */
        /* Or if not, is address in a common segment? */
        if (0
            || (regs->CR( aea_crn ) == regs->tlb.TLB_ASD( tlbix ))
            || (regs->AEA_COMMON( aea_crn ) & regs->tlb.common[ tlbix ])
        )
        {
            /* Storage Key zero? */
            /* Or if not, same Storage Key as before? */
            if (0
                || akey == 0
                || akey == regs->tlb.skey[ tlbix ]
            )
            {
                /* Does the page address match the one in the TLB? */
                /* (does a TLB entry exist for this page address?) */
                if (
                    ((addr & TLBID_PAGEMASK) | regs->tlbID)
                    ==
                    regs->tlb.TLB_VADDR( tlbix )
                )
                {
                    /* Is storage being accessed same way as before? */
                    if (acctype & regs->tlb.acc[ tlbix ])
                    {
                        /*------------------------------------------*/
                        /* TLB hit: use previously translated value */
                        /*------------------------------------------*/

                        if (acctype & ACC_CHECK)
                            regs->dat.storkey = regs->tlb.storkey[ tlbix ];

                        maddr = MAINADDR( regs->tlb.main[tlbix], addr );
                    }
                }
            }
        }
    }

    /*---------------------------------------*/
    /* TLB miss: do full address translation */
    /*---------------------------------------*/
    if (!maddr)
        maddr = ARCH_DEP( logical_to_main_l )( addr, arn, regs, acctype, akey, len );

#if defined( FEATURE_073_TRANSACT_EXEC_FACILITY )
    if (FACILITY_ENABLED( 073_TRANSACT_EXEC, regs ))
    {

  #if defined( TXF_BACKOUT_METHOD )

        // The TXF_BACKOUT_METHOD verifies any storage access to not conflict
        // with tranactional accesses on the same cache line.
        int     txf_tac;
        U32     txf_page_cache_lines_status_maddrl = TXF_PAGE_CACHE_LINES_STATUS_MADDRL( maddr, len );
        bool    txf_page_cache_lines_status_fetched;

        if (0
            || ( txf_page_cache_lines_status_maddrl & TXF_PAGE_CACHE_LINES_STATUS_STORED )
            || ( txf_page_cache_lines_status_maddrl && ( TXF_ACCTYPE( acctype ) & ACC_WRITE ) ) )
        {

            // Non-transactional conflicting accesses can only proceed after
            // any conflicting transactional stores are backed out, and the
            // txf_page_cache_lines_status bits reset.  The transaction also
            // then needs to be aborted, which will take place thereafter.
            if ( TXF_NONTRANSACTIONAL_ACCESS( regs, arn ) )
            {
                if ( txf_page_cache_lines_status_maddrl & TXF_PAGE_CACHE_LINES_STATUS_STORED )
                    PTT_TXF( "TXFB N stor cnf", maddr, txf_page_cache_lines_status_maddrl, sysblk.txf_stats[1].txf_trans );
                else
                    PTT_TXF( "TXFB N ftch cnf", maddr, txf_page_cache_lines_status_maddrl, sysblk.txf_stats[1].txf_trans );
                txf_backout_abort_cache_lines( len, regs, maddr );
            }
            else

        #if defined( TXF_COMMIT_METHOD )

                if ( !regs->txf_backout_abort_initiated )

        #endif /* defined( TXF_COMMIT_METHOD ) */

            {
                bool    txf_cache_line_in_use = FALSE ;

                // A transctional access can only cause a conflict
                // when there is more than one transaction going on.
                if ( sysblk.txf_transcpus > 1 )
                {

                    // But are we the user of the cache line we're trying to access ?
                    BYTE*   maddr_next = (BYTE*) ( (U64) maddr & ZCACHE_LINE_ADDRMASK );
                    int     i, j;

                    for ( i = 0; i < TXF_PAGE_CACHE_LINES_STATUS_SIZE( maddr, len ) && !txf_cache_line_in_use; i++ )
                    {

                        // We verify if the very same cache line was not yet recorded by this transaction.
                        for ( j = 0;
                              j < regs->txf_backout_cache_lines_count
                              && !( txf_cache_line_in_use = ( regs->txf_backout_cache_lines[ j ].maddr == maddr_next ) );
                              j++ ) {};
                        maddr_next += ZCACHE_LINE_SIZE;
                    }

                    // If we are the NOT the cache line user, then there is a conflict,
                    // as another transaction must be using it, so we need to abort ours.
                    if ( !txf_cache_line_in_use )
                    {

    #if !defined( TXF_COMMIT_METHOD )

                        // If the original TXF_COMMIT_METHOD is disabled, then this
                        // is where the transaction will be aborted.  All TXF aborts
                        // will also backout the necessary cache lines in case the
                        // TXF_BACKOUT_METHOD is enabled, thus even supporting BOTH
                        // methods to be enabled, merely for code verification purposes.
                        if ( TXF_ACCTYPE( acctype ) & ACC_WRITE )
                             txf_tac = TAC_STORE_CNF;
                        else
                             txf_tac = TAC_FETCH_CNF;
                        ABORT_TRANS( regs, ABORT_RETRY_CC, txf_tac );
                        UNREACHABLE_CODE( return maddr );

    #else

                        // When the original TXF_COMMIT_METHOD is enabled, it takes
                        // precedence over the newer TXF_BACKOUT_METHOD, so access
                        // conflicts only cause transaction aborts at TEND time,
                        // when the commit is attempted but which will then fail.
                        txf_backout_abort_cache_lines( 1, regs, NULL );

    #endif /* !defined( TXF_COMMIT_METHOD ) */

                    }
                } /* if ( sysblk.txf_transcpus > 1 ) */

                // If the number of CPU's in a transaction is one or if we are the
                // cache line user, but that cache line status is not yet stored
                // (which implies that we're now trying to store into that cache line),
                // then we still have to update that status to stored.  Unless of
                // course the status of that cache line has been changed in the
                // mean time so that it's no longer just fetch accessed.
                if (1
                    && !( txf_page_cache_lines_status_maddrl & TXF_PAGE_CACHE_LINES_STATUS_STORED )
                    &&  (0
                        || sysblk.txf_transcpus == 1
                        || txf_cache_line_in_use ) )
                {
                    txf_page_cache_lines_status_fetched = TRUE;

                    // MAINLOCK may be required if cmpxchg assists are unavailable
                    // which is unlikely and which then becomes a no-op.
                    OBTAIN_MAINLOCK( regs );
                    {
                        while (1
                              && txf_page_cache_lines_status_fetched
                              && cmpxchg4( &txf_page_cache_lines_status_maddrl,
                                            txf_page_cache_lines_status_maddrl |
                                            TXF_PAGE_CACHE_LINES_STATUS_TO_MERGE( maddr, len, ACC_WRITE ),
                                           &TXF_PAGE_CACHE_LINES_STATUS( maddr ) ) )
                        {
                        if( txf_page_cache_lines_status_maddrl != ( TXF_PAGE_CACHE_LINES_STATUS_MASK( maddr, len )
                                                                  & TXF_PAGE_CACHE_LINES_STATUS_ACCESSED ) )
                            txf_page_cache_lines_status_fetched = FALSE;
                        }
                    }
                    RELEASE_MAINLOCK( regs );
                    if ( txf_page_cache_lines_status_fetched )
                        PTT_TXF( "TXFB cl up stor", maddr, regs->txf_tac_tnd, sysblk.txf_stats[1].txf_trans );
                    else
                        PTT_TXF( "TXFB cl chg nop", maddr, regs->txf_tac_tnd, sysblk.txf_stats[1].txf_trans );
                }
            } /* if ( TXF_NONTRANSACTIONAL_ACCESS( regs, arn ) ) */
        }

        // Transactional accesses without a conflict still need an update to
        // the page cache lines status.  For store operations we will save
        // the original mainstore cache lines, so that we can backout those
        // store operations if the need would arise due to a conflict later on.
        else if ( !TXF_NONTRANSACTIONAL_ACCESS( regs, arn ) )
        {
            txf_page_cache_lines_update( len, arn, regs, acctype, maddr );
        }

    #if defined( TXF_COMMIT_METHOD )

        if ( !TXF_NONTRANSACTIONAL_ACCESS( regs, arn ) )
        {
            // If the original TXF_COMMI_METHOD is also enabled, it will take
            // precedence over the TXF_BACKOUT_METHOD, and the latter will only
            // verify matters for logic onsistency.  So TXF_MADDRL will still
            // be used to update maddr.  Please note that the backout operation
            // later on in this case is not allowed to update the original
            // mainstor, but could be used to update the TXF_MADDRL returned
            // maddr storage, after which a memcmp(...) is possible, verifying
            // that "altpage" matches the "savepage" again (in "txf_maddr_l()".

            /* Translate to alternate TXF address */
            maddr = TXF_MADDRL( addr, len, arn, regs, acctype, maddr );
        }

    #endif /* defined( TXF_COMMIT_METHOD ) */


  #else

        /* SA22-7832-12 Principles of Operation, page 5-99:

             "Storage accesses for instruction and DAT- and ART-
              table fetches follow the non-transactional rules."
        */
        if (0
            || !regs
            || !regs->txf_tnd
            || arn == USE_INST_SPACE    /* Instruction fetching */
            || arn == USE_REAL_ADDR     /* Address translation  */
        )
            return maddr;

        /* Quick exit if NTSTG call */
        if (regs->txf_NTSTG)
        {
            regs->txf_NTSTG = false;
            return maddr;
        }

        /* Translate to alternate TXF address */
        maddr = TXF_MADDRL( addr, len, arn, regs, acctype, maddr );

  #endif /* defined( TXF_BACKOUT_METHOD ) */
    }
#endif

    return maddr;
}

#if defined( FEATURE_DUAL_ADDRESS_SPACE )

U16  ARCH_DEP( translate_asn )( U16 asn, REGS* regs, U32* asteo, U32 aste[] );
int  ARCH_DEP( authorize_asn )( U16 ax, U32 aste[], int atemask, REGS* regs );

#endif

/* end of DAT.H */
