"""Offline reference execution contract; contains no game-command API.

The adapter supplies legal offers and authoritative observations. Bank snapshots
must already include every ticket listed as spent. Acceptance alone is neither
spending confirmation nor completion. Uncertain dispatches retain reservations.
"""
from dataclasses import dataclass

from .production_demands import ACTIONS, MAX_COUNT


@dataclass(frozen=True)
class Proposal:
    key: int
    action: str
    quantity: int
    expires: int


@dataclass(frozen=True)
class Offer:
    action: str
    actor: int
    minerals: int
    gas: int
    legal: bool = True
    idle: bool = True
    placement: bool = True


@dataclass
class Ticket:
    key: int
    proposal: int
    slot: int
    actor: int
    minerals: int
    gas: int
    phase: str = 'dispatch_pending'
    spent: bool = False
    actor_released: bool = False
    acceptance_confirmed: bool = False

    @property
    def holds_resources(self):
        return not self.spent and self.phase not in ('rejected','failed','completed')

    @property
    def holds_actor(self):
        return not self.actor_released and self.phase not in ('rejected','failed','completed')


class ProductionCommitments:
    def __init__(self, max_tickets=1024):
        if max_tickets<1: raise ValueError('positive ticket bound required')
        self.max_tickets=max_tickets
        self.proposals={};self.cancelled=set();self.tickets={}
        self.last_proposal=-1;self.frame=-1;self.bank=(0,0);self.live=set()
        self.enabled=True;self.last_snapshot=None

    def submit(self, proposal):
        if (proposal.key<0 or proposal.action not in ACTIONS or
                not 1<=proposal.quantity<=MAX_COUNT or proposal.expires<=self.frame):
            raise ValueError('invalid or expired proposal')
        if proposal.key in self.proposals:
            if proposal!=self.proposals[proposal.key]:
                raise ValueError('proposal IDs are immutable')
            return False
        if not self.enabled or proposal.key<=self.last_proposal:
            return False
        if len(self.proposals)>=self.max_tickets:
            self.disable();return False
        # Explicit cancellation is required before another owner for this type.
        if any(p.action==proposal.action and self.active(p) for p in self.proposals.values()):
            raise ValueError('production type already has an active proposal')
        self.proposals[proposal.key]=proposal;self.last_proposal=proposal.key
        return True

    def active(self, proposal):
        return proposal.key not in self.cancelled and proposal.expires>self.frame

    def cancel(self,key):
        if key not in self.proposals: raise ValueError('unknown proposal')
        self.cancelled.add(key)
        # Already dispatched work requires an observed outcome. Never assume
        # cancelling a model proposal cancels an issued game command.

    def disable(self):
        self.enabled=False;self.cancelled.update(self.proposals)

    def accepted(self,key):
        ticket=self.tickets[key]
        if ticket.phase=='accepted': return
        if ticket.phase not in ('dispatch_pending','uncertain'): raise ValueError('acceptance conflicts with outcome')
        ticket.phase='accepted';ticket.acceptance_confirmed=True

    def rejected(self,key):
        ticket=self.tickets[key]
        if ticket.phase=='rejected': return
        if ticket.phase not in ('dispatch_pending','uncertain') or ticket.acceptance_confirmed:
            raise ValueError('rejection conflicts with acceptance')
        ticket.phase='rejected';ticket.actor_released=True

    def observe(self,frame,minerals,gas,live_actors,*,spent=(),completed=(),failed=(),started=()):
        if min(frame,minerals,gas)<0 or frame<self.frame:
            raise ValueError('invalid/backwards snapshot; reset for a new game')
        spent,completed,failed,started=map(set,(spent,completed,failed,started))
        snapshot=(frame,minerals,gas,frozenset(live_actors),frozenset(spent),
            frozenset(completed),frozenset(failed),frozenset(started))
        if frame==self.frame:
            if snapshot==self.last_snapshot:return
            raise ValueError('conflicting same-frame observation')
        if completed&failed: raise ValueError('conflicting observed outcomes')
        # Validate the complete observation before any mutation.
        for key in spent|completed|failed|started:
            if key not in self.tickets: raise ValueError('unknown feedback ticket')
            t=self.tickets[key]
            if t.phase not in ('accepted','uncertain','completed','failed'):
                raise ValueError('feedback without accepted or uncertain dispatch')
            if key in completed and t.phase=='failed' or key in failed and t.phase=='completed':
                raise ValueError('terminal outcome changed')
            if key in (completed|failed|started) and not (t.spent or key in spent):
                raise ValueError('outcome requires reconciled spending snapshot')
        self.frame=frame;self.bank=(minerals,gas);self.live=set(live_actors);self.last_snapshot=snapshot
        for key in spent:self.tickets[key].spent=True
        for key in started:
            t=self.tickets[key]
            # The product is now independently tracked. A subsequent dispatch
            # still requires an authoritative legal and idle producer offer.
            t.actor_released=True
        for key,phase in [(k,'completed') for k in completed]+[(k,'failed') for k in failed]:
            t=self.tickets[key];t.phase=phase;t.actor_released=True
        for t in self.tickets.values():
            if t.holds_actor and t.actor not in self.live:
                # Death proves loss of the actor; it does not resolve whether
                # an ambiguous command spent money or produced another unit.
                if t.spent:t.phase='failed';t.actor_released=True
                else:t.phase='uncertain'

    def reservations(self):
        return tuple(sum(getattr(t,name) for t in self.tickets.values() if t.holds_resources)
                     for name in ('minerals','gas'))

    def reserve(self,offers):
        if not self.enabled or self.frame<0:return []
        offers=list(offers)
        if any(o.action not in ACTIONS or min(o.minerals,o.gas,o.actor)<0 for o in offers):
            raise ValueError('invalid legal offer')
        held_m,held_g=self.reservations();free_m,free_g=self.bank[0]-held_m,self.bank[1]-held_g
        busy={t.actor for t in self.tickets.values() if t.holds_actor}
        result=[]
        for p in self.proposals.values():
            if not self.active(p):continue
            for slot in range(p.quantity):
                attempts=[t for t in self.tickets.values() if t.proposal==p.key and t.slot==slot]
                if (len(attempts)>=2 or any(t.phase not in ('rejected','failed') for t in attempts)):
                    continue
                if len(self.tickets)>=self.max_tickets:
                    self.disable();return result
                offer=next((o for o in offers if o.action==p.action and o.actor in self.live
                    and o.actor not in busy and o.legal and o.idle and o.placement
                    and o.minerals<=free_m and o.gas<=free_g),None)
                if offer is None:continue
                ticket=Ticket(len(self.tickets)+1,p.key,slot,offer.actor,offer.minerals,offer.gas)
                self.tickets[ticket.key]=ticket;result.append(ticket)
                free_m-=offer.minerals;free_g-=offer.gas;busy.add(offer.actor)
        return result

    def completed(self,proposal):
        return sum(t.phase=='completed' for t in self.tickets.values() if t.proposal==proposal)
